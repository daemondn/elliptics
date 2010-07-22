/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elliptics/packet.h"
#include "elliptics/interface.h"

#include "../backends.h"
#include "blob.h"

#ifndef __unused
#define __unused	__attribute__ ((unused))
#endif

#define DNET_BLOB_INDEX_SUFFIX			".index"
#define DNET_BLOB_DEFAULT_HASH_SIZE		1024*1024*10

struct blob_backend_io {
	int			fd, index;
	unsigned int		bsize;
	off_t			offset;
	off_t			index_pos;
};

struct blob_backend
{
	unsigned int		hash_size;
	unsigned int		hash_flags;
	int			sync;

	int			iterate_threads;

	struct blob_backend_io	data, history;

	pthread_mutex_t		lock;
	struct dnet_hash	*hash;
};

struct dnet_blob_iterator_data {
	pthread_t		id;

	struct blob_backend	*b;
	struct blob_backend_io	*io;
	struct dnet_log		*log;

	size_t			num;
	off_t			pos;

	int			(* iterator)(struct blob_disk_control *dc, void *data, off_t position, void *priv);
	void			*priv;

	int			err;
};

static void *dnet_blob_iterator(void *data)
{
	struct dnet_blob_iterator_data *p = data;

	p->err = blob_iterate(p->io->index, p->pos, p->num, p->log, p->iterator, p->priv);
	if (p->err)
		dnet_backend_log(DNET_LOG_ERROR, "blob: data iteration failed: %d.\n", p->err);

	return &p->err;
};

static int dnet_blob_iterate(struct blob_backend *b, struct blob_backend_io *io, struct dnet_log *log,
	int (* iterator)(struct blob_disk_control *dc, void *data, off_t position, void *priv),
	void *priv)
{
	int i, err, error;
	int thread_num = b->iterate_threads - 1;
	struct dnet_blob_iterator_data p[thread_num + 1];
	off_t pos = 0, num = io->index_pos / b->iterate_threads;
	off_t rest = io->index_pos;

	for (i=0; i<thread_num; ++i) {
		p[i].pos = pos;
		p[i].num = num;
		p[i].b = b;
		p[i].io = io;
		p[i].iterator = iterator;
		p[i].priv = priv;
		p[i].log = log;

		err = pthread_create(&p[i].id, NULL, dnet_blob_iterator, &p[i]);
		if (err) {
			dnet_backend_log(DNET_LOG_ERROR, "blob: failed to create iterator thread: %d.\n", err);
			break;
		}

		pos += num;
		rest -= num;
	}

	p[thread_num].pos = pos;
	p[thread_num].num = rest;
	p[thread_num].b = b;
	p[thread_num].io = io;
	p[thread_num].iterator = iterator;
	p[thread_num].priv = priv;
	p[thread_num].log = log;

	dnet_blob_iterator(&p[thread_num]);

	error = p[thread_num].err;

	for (i=0; i<thread_num; ++i) {
		err = pthread_join(p[i].id, NULL);
		if (err)
			error = err;
	}

	dnet_backend_log(DNET_LOG_INFO, "blob: interation completed: num: %llu, threads: %u, status: %d.\n",
			io->index_pos, b->iterate_threads, error);

	return error;
}

static int blob_write_low_level(int fd, void *data, size_t size, size_t offset)
{
	ssize_t err = 0;

	while (size) {
		err = pwrite(fd, data, size, offset);
		if (err <= 0) {
			err = -errno;
			dnet_backend_log(DNET_LOG_ERROR, "blob: failed (%zd) to write %zu bytes into datafile: %s.\n",
					err, size, strerror(errno));
			if (!err)
				err = -EINVAL;
			goto err_out_exit;
		}

		data += err;
		size -= err;
		offset += err;
	}

	err = 0;

err_out_exit:
	return err;
}

static int blob_mark_index_removed(int fd, off_t offset, int hist)
{
	uint64_t flags = dnet_bswap64(BLOB_DISK_CTL_REMOVE);
	int err;

	err = pwrite(fd, &flags, sizeof(flags), offset + offsetof(struct blob_disk_control, flags));
	if (err != (int)sizeof(flags))
		err = -errno;

	dnet_backend_log(DNET_LOG_NOTICE, "backend: marking index entry as removed: history: %d, position: %llu (0x%llx), err: %d.\n",
			hist, (unsigned long long)offset, (unsigned long long)offset, err);
	return 0;
}

static unsigned char blob_empty_buf[40960];

static int blob_update_index(struct blob_backend *b, struct blob_ram_control *data_ctl, struct blob_ram_control *old)
{
	struct blob_disk_control dc;
	off_t *offset = &b->data.index_pos;
	int err, fd = b->data.index;

	memcpy(dc.id, data_ctl->key, DNET_ID_SIZE);
	dc.flags = 0;
	dc.data_size = data_ctl->size;
	dc.disk_size = sizeof(struct blob_disk_control);
	dc.position = data_ctl->offset;

	if (data_ctl->key[DNET_ID_SIZE]) {
		fd = b->history.index;
		dc.flags = BLOB_DISK_CTL_HISTORY;
		offset = &b->history.index_pos;
	}

	dnet_backend_log(DNET_LOG_NOTICE, "%s: updated index at position %llu (0x%llx), data position: %llu (0x%llx), data size: %llu.\n",
			dnet_dump_id(data_ctl->key),
			(unsigned long long)(*offset)*sizeof(dc), (unsigned long long)(*offset)*sizeof(dc),
			(unsigned long long)data_ctl->offset, (unsigned long long)data_ctl->offset,
			data_ctl->size);

	blob_convert_disk_control(&dc);

	err = pwrite(fd, &dc, sizeof(dc), (*offset)*sizeof(dc));
	if (err != (int)sizeof(dc)) {
		err = -errno;
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to write index data at %llu: %s.\n",
			dnet_dump_id(data_ctl->key), (unsigned long long)(*offset)*sizeof(dc), strerror(errno));
		goto err_out_exit;
	}

	*offset = *offset + 1;
	err = 0;

	if (old) {
		blob_mark_index_removed(fd, old->index_pos * sizeof(dc), data_ctl->key[DNET_ID_SIZE]);
		if (data_ctl->key[DNET_ID_SIZE])
			fd = b->history.fd;
		else
			fd = b->data.fd;

		blob_mark_index_removed(fd, old->offset, data_ctl->key[DNET_ID_SIZE]);
	}

err_out_exit:
	return err;
}

static int blob_write_raw(struct blob_backend *b, int hist, struct dnet_io_attr *io, void *data)
{
	ssize_t err;
	int fd, bsize, have_old = 0;
	struct blob_disk_control disk_ctl;
	struct blob_ram_control ctl, old;
	unsigned int dsize = sizeof(old);
	off_t offset;
	size_t disk_size;

	memcpy(disk_ctl.id, io->origin, DNET_ID_SIZE);

	memcpy(ctl.key, io->origin, DNET_ID_SIZE);
	ctl.key[DNET_ID_SIZE] = !!hist;

	pthread_mutex_lock(&b->lock);

	disk_ctl.flags = 0;

	if (hist) {
		fd = b->history.fd;
		ctl.offset = b->history.offset;
		ctl.index_pos = b->history.index_pos;
		bsize = b->history.bsize;
		disk_ctl.flags = BLOB_DISK_CTL_HISTORY;
	} else {
		fd = b->data.fd;
		ctl.offset = b->data.offset;
		ctl.index_pos = b->data.index_pos;
		bsize = b->data.bsize;
	}

	disk_ctl.position = ctl.offset;
	disk_ctl.data_size = io->size;
	disk_ctl.disk_size = io->size + sizeof(struct blob_disk_control);
	if (bsize)
		disk_ctl.disk_size = ALIGN(disk_ctl.disk_size, bsize);

	blob_convert_disk_control(&disk_ctl);

	offset = ctl.offset;
	err = blob_write_low_level(fd, &disk_ctl, sizeof(struct blob_disk_control), offset);
	if (err)
		goto err_out_unlock;
	offset += sizeof(struct blob_disk_control);

	err = blob_write_low_level(fd, data, io->size, offset);
	if (err)
		goto err_out_unlock;
	offset += io->size;

	if (bsize) {
		int size = bsize - ((offset - ctl.offset) % bsize);

		while (size && size < bsize) {
			unsigned int sz = size;

			if (sz > sizeof(blob_empty_buf))
				sz = sizeof(blob_empty_buf);

			err = blob_write_low_level(fd, blob_empty_buf, sz, offset);
			if (err)
				goto err_out_unlock;

			size -= sz;
			offset += sz;
		}
	}
	disk_size = offset - ctl.offset;
	ctl.size = io->size;

	err = dnet_hash_lookup(b->hash, ctl.key, sizeof(ctl.key), &old, &dsize);
	if (!err)
		have_old = 1;

	err = dnet_hash_replace(b->hash, ctl.key, sizeof(ctl.key), &ctl, sizeof(ctl));
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: %s: failed to add hash entry: %s [%d].\n",
				dnet_dump_id(io->origin), strerror(-err), err);
		goto err_out_unlock;
	}

	if (hist)
		b->history.offset += disk_size;
	else
		b->data.offset += disk_size;

	err = blob_update_index(b, &ctl, have_old ? &old : NULL);
	if (err)
		goto err_out_unlock;

	dnet_backend_log(DNET_LOG_INFO, "blob: %s: written history: %d, position: %zu, size: %llu, on-disk-size: %zu.\n",
			dnet_dump_id(io->origin), hist, ctl.offset, (unsigned long long)io->size, disk_size);

err_out_unlock:
	pthread_mutex_unlock(&b->lock);
	return err;
}

static int blob_write_history_meta(void *state, void *backend, struct dnet_io_attr *io,
		struct dnet_meta *m, void *data)
{
	struct blob_backend *b = backend;
	struct blob_ram_control ctl;
	unsigned char key[DNET_ID_SIZE + 1];
	unsigned int dsize = sizeof(struct blob_ram_control);
	void *hdata, *new_hdata;
	uint64_t saved_io_size = io->size;
	size_t size = 0;
	int err;

	memcpy(key, io->origin, DNET_ID_SIZE);
	key[DNET_ID_SIZE] = 1;

	err = dnet_hash_lookup(b->hash, key, sizeof(key), &ctl, &dsize);
	if (!err)
		size = ctl.size + sizeof(struct blob_disk_control);

	hdata = malloc(size);
	if (!hdata) {
		err = -ENOMEM;
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to allocate %zu bytes for history data: %s.\n",
				dnet_dump_id(key), size, strerror(errno));
		goto err_out_exit;
	}

	if (!err) {
		struct blob_disk_control *dc;

		dnet_backend_log(DNET_LOG_INFO,	"%s: found existing block at: %llu, size: %zu.\n",
			dnet_dump_id(key), (unsigned long long)ctl.offset, size);

		err = pread(b->history.fd, hdata, size, ctl.offset);
		if (err != (int)size) {
			err = -errno;
			dnet_backend_log(DNET_LOG_ERROR, "%s: failed to read %zu bytes from history at %llu: %s.\n",
				dnet_dump_id(key), size, (unsigned long long)ctl.offset, strerror(errno));
			goto err_out_free;
		}

		dc = hdata;

		blob_convert_disk_control(dc);
		dc->flags |= BLOB_DISK_CTL_REMOVE;
		size = dc->data_size;
		blob_convert_disk_control(dc);

		err = pwrite(b->history.fd, dc, sizeof(struct blob_disk_control), ctl.offset);
		if (err != (int)sizeof(*dc)) {
			err = -errno;
			dnet_backend_log(DNET_LOG_ERROR, "%s: failed to erase (mark) history entry at %llu: %s.\n",
				dnet_dump_id(key), (unsigned long long)ctl.offset, strerror(errno));
			goto err_out_free;
		}

		memmove(hdata, dc + 1, size);
	}

	new_hdata = backend_process_meta(state, io, hdata, (uint32_t *)&size, m, data);
	if (!new_hdata) {
		err = -ENOMEM;
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to update history file: %s.\n",
				dnet_dump_id(key), strerror(errno));
		goto err_out_free;
	}
	hdata = new_hdata;

	io->size = size;
	err = blob_write_raw(b, 1, io, new_hdata);
	io->size = saved_io_size;
	if (err) {
		err = -errno;
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to update (%zu bytes) history: %s.\n",
				dnet_dump_id(key), size, strerror(errno));
		goto err_out_free;
	}

	err = 0;

err_out_free:
	free(hdata);
err_out_exit:
	return err;
}

static int blob_write_history(struct blob_backend *b, void *state, struct dnet_io_attr *io, void *data)
{
	return backend_write_history(state, b, io, data, blob_write_history_meta);
}

static int blob_write(struct blob_backend *r, void *state, struct dnet_cmd *cmd,
		struct dnet_attr *attr __unused, void *data)
{
	int err;
	struct dnet_io_attr *io = data;

	dnet_convert_io_attr(io);

	data += sizeof(struct dnet_io_attr);

	if (io->flags & DNET_IO_FLAGS_HISTORY) {
		err = blob_write_history(r, state, io, data);
		if (err)
			goto err_out_exit;
	} else {
		err = blob_write_raw(r, 0, io, data);
		if (err)
			goto err_out_exit;

		if (!(io->flags & DNET_IO_FLAGS_NO_HISTORY_UPDATE)) {
			struct dnet_history_entry e;

			dnet_setup_history_entry(&e, io->id, io->size, io->offset, NULL, io->flags);

			io->flags |= DNET_IO_FLAGS_APPEND | DNET_IO_FLAGS_HISTORY;
			io->flags &= ~DNET_IO_FLAGS_META;
			io->size = sizeof(struct dnet_history_entry);
			io->offset = 0;

			err = blob_write_history(r, state, io, &e);
			if (err)
				goto err_out_exit;
		}
	}

	dnet_backend_log(DNET_LOG_NOTICE, "blob: %s: IO offset: %llu, size: %llu.\n", dnet_dump_id(cmd->id),
		(unsigned long long)io->offset, (unsigned long long)io->size);

	return 0;

err_out_exit:
	return err;
}

static int blob_read(struct blob_backend *b, void *state, struct dnet_cmd *cmd,
		struct dnet_attr *attr, void *data)
{
	struct dnet_io_attr *io = data;
	struct blob_ram_control ctl;
	unsigned char key[DNET_ID_SIZE + 1];
	unsigned long long size = io->size;
	unsigned int dsize = sizeof(struct blob_ram_control);
	off_t offset;
	int fd, err;

	data += sizeof(struct dnet_io_attr);

	dnet_convert_io_attr(io);

	memcpy(key, io->origin, DNET_ID_SIZE);
	key[DNET_ID_SIZE] = !!(io->flags & DNET_IO_FLAGS_HISTORY);
	fd = (io->flags & DNET_IO_FLAGS_HISTORY) ? b->history.fd : b->data.fd;

	err = dnet_hash_lookup(b->hash, key, sizeof(key), &ctl, &dsize);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: %s: could not find data: %d.\n",
				dnet_dump_id(io->origin), err);
		goto err_out_exit;
	}

	if (!size)
		size = ctl.size;

	offset = ctl.offset + sizeof(struct blob_disk_control) + io->offset;

	if (attr->size == sizeof(struct dnet_io_attr)) {
		struct dnet_data_req *r;
		struct dnet_cmd *c;
		struct dnet_attr *a;
		struct dnet_io_attr *rio;

		r = dnet_req_alloc(state, sizeof(struct dnet_cmd) +
				sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr));
		if (!r) {
			err = -ENOMEM;
			dnet_backend_log(DNET_LOG_ERROR, "%s: failed to allocate reply attributes.\n",
					dnet_dump_id(io->origin));
			goto err_out_exit;
		}

		dnet_req_set_fd(r, fd, offset, size, 0);

		c = dnet_req_header(r);
		a = (struct dnet_attr *)(c + 1);
		rio = (struct dnet_io_attr *)(a + 1);

		memcpy(c->id, io->origin, DNET_ID_SIZE);
		memcpy(rio->origin, io->origin, DNET_ID_SIZE);

		dnet_backend_log(DNET_LOG_NOTICE, "%s: read: requested offset: %llu, size: %llu, "
				"stored-size: %llu, data lives at: %zu.\n",
				dnet_dump_id(io->origin), (unsigned long long)io->offset,
				size, (unsigned long long)ctl.size, ctl.offset);

		if (cmd->flags & DNET_FLAGS_NEED_ACK)
			c->flags = DNET_FLAGS_MORE;

		c->status = 0;
		c->size = sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr) + size;
		c->trans = cmd->trans | DNET_TRANS_REPLY;

		a->cmd = DNET_CMD_READ;
		a->size = sizeof(struct dnet_io_attr) + size;
		a->flags = attr->flags;

		rio->size = size;
		rio->offset = io->offset;
		rio->flags = io->flags;

		dnet_convert_cmd(c);
		dnet_convert_attr(a);
		dnet_convert_io_attr(rio);

		err = dnet_data_ready(state, r);
		if (err)
			goto err_out_exit;
	} else {
		if (size > attr->size - sizeof(struct dnet_io_attr))
			size = attr->size - sizeof(struct dnet_io_attr);

		err = pread(fd, data, size, offset);
		if (err <= 0) {
			err = -errno;
			dnet_backend_log(DNET_LOG_ERROR, "%s: failed to read object data: %s.\n",
					dnet_dump_id(io->origin), strerror(errno));
			goto err_out_exit;
		}

		io->size = err;
		attr->size = sizeof(struct dnet_io_attr) + io->size;
	}

	return 0;

err_out_exit:
	return err;
}

static int blob_del_entry(struct blob_backend *b, struct dnet_cmd *cmd, int hist)
{
	unsigned char key[DNET_ID_SIZE + 1];
	struct blob_ram_control ctl;
	unsigned int dsize = sizeof(struct blob_ram_control);
	struct blob_disk_control dc;
	int err, fd = b->data.fd;

	memcpy(key, cmd->id, DNET_ID_SIZE);
	key[DNET_ID_SIZE] = 0;

	if (hist) {
		key[DNET_ID_SIZE] = 1;
		fd = b->history.fd;
	}

	err = dnet_hash_lookup(b->hash, key, sizeof(key), &ctl, &dsize);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: %s: could not find data to be removed: %d.\n",
				dnet_dump_id(key), err);
		goto err_out_exit;
	}

	dnet_backend_log(DNET_LOG_INFO,	"%s: removing block at: %llu, size: %llu.\n",
		dnet_dump_id(key), (unsigned long long)ctl.offset, (unsigned long long)ctl.size);

	err = pread(fd, &dc, sizeof(dc), ctl.offset);
	if (err != (int)sizeof(dc)) {
		err = -errno;
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to read disk control structure from history at %llu: %s.\n",
			dnet_dump_id(key), (unsigned long long)ctl.offset, strerror(errno));
		goto err_out_exit;
	}

	blob_convert_disk_control(&dc);
	dc.flags |= BLOB_DISK_CTL_REMOVE;
	blob_convert_disk_control(&dc);

	err = pwrite(fd, &dc, sizeof(struct blob_disk_control), ctl.offset);
	if (err != (int)sizeof(dc)) {
		err = -errno;
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to erase (mark) entry at %llu: %s.\n",
			dnet_dump_id(key), (unsigned long long)ctl.offset, strerror(errno));
		goto err_out_exit;
	}
	err = 0;

	blob_mark_index_removed((hist) ? b->history.index : b->data.index, ctl.offset, hist);

err_out_exit:
	return err;
}

static int blob_del(struct blob_backend *b, struct dnet_cmd *cmd)
{
	int err;

	err = blob_del_entry(b, cmd, 0);
	err = blob_del_entry(b, cmd, 1);

	return err;
}

struct blob_iterate_shared {
	struct blob_backend	*b;
	void			*state;
	struct dnet_cmd		*cmd;
	struct dnet_attr	*attr;
	unsigned char		id[DNET_ID_SIZE];

	int			pos;

	pthread_mutex_t		lock;
	struct dnet_id		ids[10240];
};

static int blob_iterate_list_callback(struct blob_disk_control *dc, void *data, off_t position __unused, void *priv)
{
	struct blob_iterate_shared *s = priv;
	struct dnet_history_entry *e;
	struct dnet_meta *m;
	int err = 0;

	if (s->attr->flags & DNET_ATTR_ID_OUT) {
		if (!dnet_id_within_range(dc->id, s->id, s->cmd->id))
			goto err_out_exit;
	}

	if (dc->flags & BLOB_DISK_CTL_REMOVE)
		goto err_out_exit;

	m = dnet_meta_search(NULL, data, dc->data_size, DNET_META_HISTORY);
	if (!m) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: failed to locate history metadata.\n",
				dnet_dump_id(dc->id));
		goto err_out_exit;
	}

	if (!m->size)
		goto err_out_exit;

	if (m->size % sizeof(struct dnet_history_entry)) {
		dnet_backend_log(DNET_LOG_ERROR, "%s: Corrupted history object, "
				"its history metadata size %llu has to be modulo of %zu.\n",
				dnet_dump_id(dc->id), (unsigned long long)m->size,
				sizeof(struct dnet_history_entry));
		err = -EINVAL;
		goto err_out_exit;
	}

	e = (struct dnet_history_entry *)m->data;

	dnet_backend_log(DNET_LOG_INFO, "%s: flags: %08x\n", dnet_dump_id(dc->id), e->flags);

	pthread_mutex_lock(&s->lock);

	if (s->pos == ARRAY_SIZE(s->ids)) {
		err = dnet_send_reply(s->state, s->cmd, s->attr, s->ids, s->pos * sizeof(struct dnet_id), 1);
		if (!err)
			s->pos = 0;
	}

	if (s->pos < (int)ARRAY_SIZE(s->ids)) {
		memcpy(s->ids[s->pos].id, dc->id, DNET_ID_SIZE);
		s->ids[s->pos].flags = dnet_bswap32(e->flags);
		dnet_convert_id(&s->ids[s->pos]);
		s->pos++;
	}

	pthread_mutex_unlock(&s->lock);

err_out_exit:
	return err;
}

static int blob_list(struct blob_backend *b, void *state, struct dnet_cmd *cmd, struct dnet_attr *attr)
{
	struct blob_iterate_shared s;
	int err;

	s.b = b;
	s.state = state;
	s.cmd = cmd;
	s.attr = attr;
	s.pos = 0;

	err = pthread_mutex_init(&s.lock, NULL);
	if (err)
		goto err_out_exit;

	if (attr->flags & DNET_ATTR_ID_OUT)
		dnet_state_get_next_id(state, s.id);

	err = dnet_blob_iterate(b, &b->history, NULL, blob_iterate_list_callback, &s);
	if (err)
		goto err_out_lock_destroy;

	if (s.pos)
		err = dnet_send_reply(s.state, s.cmd, s.attr, s.ids, s.pos * sizeof(struct dnet_id), 1);

err_out_lock_destroy:
	pthread_mutex_destroy(&s.lock);
err_out_exit:
	return err;
}

static int blob_backend_command_handler(void *state, void *priv,
		struct dnet_cmd *cmd, struct dnet_attr *attr, void *data)
{
	int err;
	struct blob_backend *b = priv;

	switch (attr->cmd) {
		case DNET_CMD_WRITE:
			err = blob_write(b, state, cmd, attr, data);
			break;
		case DNET_CMD_READ:
			err = blob_read(b, state, cmd, attr, data);
			break;
		case DNET_CMD_LIST:
			err = blob_list(b, state, cmd, attr);
			break;
		case DNET_CMD_STAT:
			err = backend_stat(state, NULL, cmd, attr);
			break;
		case DNET_CMD_DEL:
			err = blob_del(b, cmd);
			break;
		default:
			err = -EINVAL;
			break;
	}

	return err;
}

static int dnet_blob_set_sync(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct blob_backend *r = b->data;

	r->sync = atoi(value);
	return 0;
}

static int dnet_blob_open_file(char *file, off_t *off_ptr)
{
	int fd, err = 0;
	off_t offset;

	fd = open(file, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		err = -errno;
		dnet_backend_log(DNET_LOG_ERROR, "Failed to open file '%s': %s.\n", file, strerror(errno));
		goto err_out_exit;
	}

	offset = lseek(fd, 0, SEEK_END);
	if (offset == (off_t) -1) {
		dnet_backend_log(DNET_LOG_ERROR, "Failed to determine file's '%s' size: %s.\n", file, strerror(errno));
		goto err_out_close;
	}

	posix_fadvise(fd, 0, offset, POSIX_FADV_SEQUENTIAL);

	*off_ptr = offset;
	return fd;

err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static int dnet_blob_open_files(char *path, struct blob_backend_io *io)
{
	char index[strlen(path)+sizeof(DNET_BLOB_INDEX_SUFFIX) + 1]; /* 0-byte */
	int err;

	io->fd = dnet_blob_open_file(path, &io->offset);
	if (io->fd < 0) {
		err = io->fd;
		goto err_out_exit;
	}

	sprintf(index, "%s%s", path, DNET_BLOB_INDEX_SUFFIX);

	io->index = dnet_blob_open_file(index, &io->index_pos);
	if (io->index < 0) {
		err = io->fd;
		goto err_out_close;
	}

	io->index_pos = io->index_pos / sizeof(struct blob_disk_control);

	dnet_backend_log(DNET_LOG_ERROR, "file: %s, size: %llu, indexed %llu entries.\n",
			path, io->offset, io->index_pos);
	return 0;

err_out_close:
	close(io->fd);
err_out_exit:
	return err;
}

static void dnet_blob_close_files(struct blob_backend_io *io)
{
	close(io->fd);
	close(io->index);
}

static int dnet_blob_set_data(struct dnet_config_backend *b, char *key, char *file)
{
	struct blob_backend *r = b->data;
	int err;

	if (!strcmp(key, "data")) {
		err = dnet_blob_open_files(file, &r->data);
	} else if (!strcmp(key, "history")) {
		err = dnet_blob_open_files(file, &r->history);
	} else {
		err = -EINVAL;
	}

	return err;
}

static int dnet_blob_set_block_size(struct dnet_config_backend *b, char *key, char *value)
{
	struct blob_backend *r = b->data;

	if (!strcmp(key, "data_block_size"))
		r->data.bsize = strtoul(value, NULL, 0);
	else
		r->history.bsize = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_iterate_thread_num(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct blob_backend *r = b->data;

	r->iterate_threads = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_hash_size(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct blob_backend *r = b->data;

	r->hash_size = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_set_hash_flags(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct blob_backend *r = b->data;

	r->hash_flags = strtoul(value, NULL, 0);
	return 0;
}

static int dnet_blob_iter(struct blob_disk_control *dc, void *data __unused, off_t position __unused, void *priv, int hist)
{
	struct blob_backend *b = priv;
	struct blob_ram_control ctl;
	char id[DNET_ID_SIZE*2+1];
	int err;

	dnet_backend_log(DNET_LOG_NOTICE, "%s (hist: %d): index position: %llu (0x%llx), data position: %llu (0x%llx), "
			"data size: %llu, disk size: %llu, flags: %llx.\n",
			dnet_dump_id_len_raw(dc->id, DNET_ID_SIZE, id), hist,
			(unsigned long long)position, (unsigned long long)position,
			(unsigned long long)dc->position, (unsigned long long)dc->position,
			(unsigned long long)dc->data_size, (unsigned long long)dc->disk_size,
			(unsigned long long)dc->flags);

	if (dc->flags & BLOB_DISK_CTL_REMOVE)
		return 0;

	memcpy(ctl.key, dc->id, DNET_ID_SIZE);
	ctl.key[DNET_ID_SIZE] = hist;
	ctl.index_pos = position / sizeof(struct blob_disk_control);
	ctl.offset = dc->position;
	ctl.size = dc->data_size;

	err = dnet_hash_replace(b->hash, ctl.key, sizeof(ctl.key), &ctl, sizeof(ctl));
	if (err)
		return err;

	return 0;
}

static int dnet_blob_iter_history(struct blob_disk_control *dc, void *data, off_t position, void *priv)
{
	return dnet_blob_iter(dc, data, position, priv, 1);
}

static int dnet_blob_iter_data(struct blob_disk_control *dc, void *data, off_t position, void *priv)
{
	return dnet_blob_iter(dc, data, position, priv, 0);
}

static int dnet_blob_config_init(struct dnet_config_backend *b, struct dnet_config *c)
{
	struct blob_backend *r = b->data;
	int err;

	if (!r->data.fd || !r->history.fd) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: no data/history file present. Exiting.\n");
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!r->iterate_threads)
		r->iterate_threads = 1;

	err = pthread_mutex_init(&r->lock, NULL);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "Failed to initialize pthread mutex: %d\n", err);
		err = -errno;
		goto err_out_close;
	}

	if (!r->hash_size)
		r->hash_size = DNET_BLOB_DEFAULT_HASH_SIZE;

	r->hash = dnet_hash_init(r->hash_size, r->hash_flags);
	if (!r->hash) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: failed to initialize hash table: num: %u, flags: 0x%x.\n",
				r->hash_size, r->hash_flags);
		err = -EINVAL;
		goto err_out_lock_destroy;
	}
	
	err = dnet_blob_iterate(r, &r->data, b->log, dnet_blob_iter_data, r);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: history iteration failed: %d.\n", err);
		goto err_out_hash_destroy;
	}
	posix_fadvise(r->data.fd, 0, r->data.offset, POSIX_FADV_RANDOM);

	err = dnet_blob_iterate(r, &r->history, b->log, dnet_blob_iter_history, r);
	if (err) {
		dnet_backend_log(DNET_LOG_ERROR, "blob: history iteration failed: %d.\n", err);
		goto err_out_hash_destroy;
	}
	posix_fadvise(r->history.fd, 0, r->history.offset, POSIX_FADV_RANDOM);

	c->command_private = b->data;
	c->command_handler = blob_backend_command_handler;

	return 0;

err_out_hash_destroy:
	dnet_hash_exit(r->hash);
err_out_lock_destroy:
	pthread_mutex_destroy(&r->lock);
err_out_close:
	dnet_blob_close_files(&r->data);
	dnet_blob_close_files(&r->history);
err_out_exit:
	return err;
}

static void dnet_blob_config_cleanup(struct dnet_config_backend *b)
{
	struct blob_backend *r = b->data;

	dnet_hash_exit(r->hash);

	dnet_blob_close_files(&r->data);
	dnet_blob_close_files(&r->history);

	pthread_mutex_destroy(&r->lock);
}

static struct dnet_config_entry dnet_cfg_entries_blobsystem[] = {
	{"sync", dnet_blob_set_sync},
	{"data", dnet_blob_set_data},
	{"history", dnet_blob_set_data},
	{"data_block_size", dnet_blob_set_block_size},
	{"history_block_size", dnet_blob_set_block_size},
	{"hash_table_size", dnet_blob_set_hash_size},
	{"hash_table_flags", dnet_blob_set_hash_flags},
	{"iterate_thread_num", dnet_blob_set_iterate_thread_num},
};

static struct dnet_config_backend dnet_blob_backend = {
	.name			= "blob",
	.ent			= dnet_cfg_entries_blobsystem,
	.num			= ARRAY_SIZE(dnet_cfg_entries_blobsystem),
	.size			= sizeof(struct blob_backend),
	.init			= dnet_blob_config_init,
	.cleanup		= dnet_blob_config_cleanup,
};

int dnet_blob_backend_init(void)
{
	return dnet_backend_register(&dnet_blob_backend);
}

void dnet_blob_backend_exit(void)
{
	/* cleanup routing will be called explicitly through backend->cleanup() callback */
}