#!/usr/bin/python
import subprocess
import time
import tarfile
import os
import sys
import shutil


def run_test(path, test):
    os.mkdir(path)
    p = subprocess.Popen([test, '--path', path], stdout=sys.stdout,
                         stderr=subprocess.STDOUT, cwd=path)
    p.communicate()

    return p.returncode


def force_mkdir(path):
    if os.path.exists(path):
        print('Removing path: {0}'.format(path))
        shutil.rmtree(path)

    os.mkdir(path)


def main():
    source_dir = sys.argv[1]
    binary_dir = sys.argv[2]

    tests = [
        (binary_dir, 'dnet_cpp_test'),
        (binary_dir, 'dnet_cpp_cache_test'),
        (binary_dir, 'dnet_cpp_srw_test'),
        (binary_dir, 'dnet_cpp_api_test')
    ]
    print('Running {0} tests'.format(len(tests)))

    tests_base_dir = binary_dir + '/result'
    force_mkdir(tests_base_dir)

    artifacts_dir = source_dir + '/artifacts'
    force_mkdir(artifacts_dir)

    all_ok = True
    for i in xrange(0, len(tests)):
        test = tests[i]
        print('# Start {1} of {2}: {0}: '.format(test[1], i + 1, len(tests)))

        timer_begin = time.time()
        result = run_test(tests_base_dir + '/' + test[1],
                          test[0] + '/' + test[1])
        timer_end = time.time()

        if result == 0:
            str_result = 'Passed'
        else:
            str_result = 'Failed ({0})'.format(result)

        print('# Result: {0}\t{1} sec\n'.format(str_result,
                                                timer_end - timer_begin))

        all_ok &= result == 0

        file = tarfile.TarFile.open(artifacts_dir + '/' + test[1] + '.tar.bz2',
                                    'w:bz2')
        file.add(tests_base_dir + '/' + test[1], test[1])
        file.close()

    print('Tests are finised')

    if all_ok:
        exit(0)
    else:
        exit(1)

if __name__ == "__main__":
    main()
