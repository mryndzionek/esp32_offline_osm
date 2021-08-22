import sys
import os
import math
import subprocess
import hashlib

from functools import partial
from PIL import Image, ImageDraw
from progress.bar import Bar
from multiprocessing import Pool, Lock


def worker(rootdir, args, a):
    subdir, f = a

    in_name = os.path.join(subdir, f)
    out_name = '.'.join(f.split('.')[:-1])
    out_name = os.path.join(subdir.replace(
        rootdir, rootdir + '_lvgl'), out_name)
    arg = '&'.join(['img={}'.format(in_name), 'name={}'.format(out_name)])
    cmd = arg + '&' + args

    lock.acquire()
    if not os.path.exists(os.path.dirname(out_name)):
        os.makedirs(os.path.dirname(out_name))
    lock.release()
    ret = subprocess.run(["php", "lv_utils/img_conv_core.php", cmd])
    if ret.returncode == 0:
        hasher = hashlib.sha1()
        with open(out_name + '.bin', 'rb') as bi:
            buf = bi.read()
            hasher.update(buf)
            return out_name, hasher.hexdigest()
    else:
        return None


def init(l):
    global lock
    lock = l


def convert_to_lvgl(rootdir, args="cf=true_color&format=bin_565_swap"):

    tiles = []
    for subdir, dirs, files in os.walk(rootdir):
        for f in files:
            tiles.append((subdir, f))

    print('{} tiles files to process'.format(len(tiles)))

    l = Lock()
    w = partial(worker, rootdir, args)

    hash2fn = {}

    with Bar('Processing', max=len(tiles)) as bar:
        with Pool(initializer=init, initargs=(l,)) as pool:
            for fn, hash in pool.imap_unordered(w, tiles):
                bar.next()
                if hash:
                    if hash in hash2fn:
                        hash2fn[hash].append(fn)
                    else:
                        hash2fn[hash] = [fn]
                else:
                    print('Worker process exited without hash')

    duplicates = [(k, v) for k, v in hash2fn.items() if len(v) > 1]
    print('{} duplicate tiles'.format(len(duplicates)))
    print([(h, len(n)) for h, n in duplicates])


if len(sys.argv) != 2:
    print('Please provide the (root) directory with tiles')
    sys.exit(1)

dirname = sys.argv[1]
convert_to_lvgl(dirname)
