import os
import sys
import math
import struct

import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

TILE_SIZE = 256


def deg2num(lat, lon, zoom):
    lat_rad = math.radians(lat)
    n = 2.0 ** zoom
    xtile = (lon + 180.0) / 360.0 * n
    ytile = (1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n
    xp = int(TILE_SIZE * (xtile % 1))
    yp = int(TILE_SIZE * (ytile % 1))
    return (int(xtile), int(n) - int(ytile) - 1, xp, yp)


def map_back(p):
    pts = []

    dx = (p[0] % TILE_SIZE)
    dy = (p[1] % TILE_SIZE)

    x = (p[0] // TILE_SIZE)
    y = (p[1] // TILE_SIZE) + 1

    if dx == 0:
        pts.append((0, TILE_SIZE - dy, x, y))
        pts.append((TILE_SIZE, TILE_SIZE - dy, x - 1, y))

    if dy == 0:
        pts.append((dx, TILE_SIZE, x, y))
        pts.append((dx, 0, x, y - 1))

    return pts


def get_patch(beg, end):
    p1x = beg[2]
    p1y = -beg[3]
    p2x = end[2] + ((end[0] - beg[0]) * TILE_SIZE)
    p2y = -end[3] + ((end[1] - beg[1]) * TILE_SIZE)

    # print('beg: {}, end: {}'.format(beg, end))
    # print('p1: {}, p2: {}'.format((p1x, p1y), (p2x, p2y)))

    dir_x = 1 if p1x < p2x else -1
    dir_y = 1 if p1y < p2y else -1
    #print('dir_x: {}, dir_y: {}'.format(dir_x, dir_y))

    hs = []
    vs = []

    has_x, has_y = False, False

    if p2x == p1x:
        s2x = p1x
        s2y = 0 if dir_y > 0 else -TILE_SIZE
        dx = 0
        has_y = True

    if p2y == p1y:
        s1y = p1y
        s1x = TILE_SIZE if dir_x > 0 else 0
        dy = 0
        has_x = True

    if not ((p2x == p1x) or (p2y == p1y)):
        a = (p2y - p1y) / (p2x - p1x)
        b = p1y - (a * p1x)
        dx = dir_x * abs(TILE_SIZE / a)
        dy = dir_y * abs(TILE_SIZE * a)
        #print('a: {}, b: {}, dx: {}, dy: {}'.format(a, b, dx, dy))

        s1x = TILE_SIZE if dir_x > 0 else 0
        s2y = 0 if dir_y > 0 else -TILE_SIZE

        s1y = (a * s1x) + b
        s2x = (s2y - b) / a
        #print('s1: {}, s2: {}'.format((s1x, s1y), (s2x, s2y)))
        has_x, has_y = True, True

    if has_x:
        while (s1x <= p2x if dir_x > 0 else s1x >= p2x) and \
                (s1y <= p2y if dir_y > 0 else s1y >= p2y):
            hs.append((round(s1x), round(s1y)))
            s1x += dir_x * TILE_SIZE
            s1y += dy

    if has_y:
        while (s2x <= p2x if dir_x > 0 else s2x >= p2x) and \
                (s2y <= p2y if dir_y > 0 else s2y >= p2y):
            vs.append((round(s2x), round(s2y)))
            s2x += dx
            s2y += dir_y * TILE_SIZE

    pts = vs + hs
    return [b for a in map(map_back, pts) for b in a]


def save_zxy(path, name):

    for (z, x, y), cs in path.items():
        fp = os.path.join(name, str(z), str(x))
        if not os.path.exists(fp):
            os.makedirs(fp)

        fn = os.path.join(fp, str(y) + '.bin')
        data = struct.pack('H', len(cs))

        for p in cs:
            data += struct.pack('H', len(p))
            for b in p:
                data += struct.pack('HH', b[0], b[1])

        with open(fn, 'wb') as f:
            f.write(data)


def draw_circles(c, ps, w=5):
    x, y = ps[0]
    c.ellipse([(x - w, y - w), (x + w, y + w)], fill='green')

    x, y = ps[-1]
    c.ellipse([(x - w, y - w), (x + w, y + w)], fill='blue')

    for (x, y) in ps[1:-1]:
        c.ellipse([(x - w, y - w), (x + w, y + w)], fill='red')


def draw_path(pts, name, zoom=16):
    a = [(x, y) for z, x, y in pts.keys() if z == zoom]
    max_x = max(a, key=lambda x: x[0])[0]
    min_x = min(a, key=lambda x: x[0])[0]
    max_y = max(a, key=lambda x: x[1])[1]
    min_y = min(a, key=lambda x: x[1])[1]

    scale = 2

    canvas_size = (scale * TILE_SIZE * (max_x - min_x + 1),
                   scale * TILE_SIZE * (max_y - min_y + 1))
    img = Image.new("RGB", canvas_size, color='white')
    canvas = ImageDraw.Draw(img)

    for x in range(0, canvas_size[0], scale * TILE_SIZE):
        canvas.line([(x, 0), (x, canvas_size[1])], width=1, fill='gray')

    for y in range(0, canvas_size[1], scale * TILE_SIZE):
        canvas.line([(0, y), (canvas_size[0], y)], width=1, fill='gray')

    for k, v in pts.items():
        z, x, y = k
        if z == zoom:
            for p in v:
                p = list(map(lambda xy: (scale * xy[0] + scale * TILE_SIZE * (
                    x - min_x), scale * xy[1] + scale * TILE_SIZE * (max_y - y)), p))
                if len(p) > 1:
                    canvas.line(p, fill='black', width=2)
                draw_circles(canvas, p)

    img.save(name + '.png')


ns = {'gpx': 'http://www.topografix.com/GPX/1/1'}

if len(sys.argv[1:]) < 1:
    print('Please provide GPX file name')
    sys.exit(1)

tree = ET.parse(sys.argv[1])
root = tree.getroot()

path = {}
trkpts = root.findall('gpx:trk/gpx:trkseg/gpx:trkpt', ns)

for z in range(0, 17):
    init_loc = (float(trkpts[0].attrib['lat']), float(trkpts[0].attrib['lon']))
    prev_x, prev_y, prev_dx, prev_dy = deg2num(*(init_loc + (z,)))
    path[(z, prev_x, prev_y)] = [[(prev_dx, prev_dy)]]

    for trkpt in trkpts[1:]:
        lat, lon = float(trkpt.attrib['lat']), float(trkpt.attrib['lon'])
        x, y, dx, dy = deg2num(lat, lon, z)

        if (x, y) == (prev_x, prev_y):
            # we are still in the same tile
            # just add the path point
            path[(z, x, y)][-1].append((dx, dy))

        else:
            # we are at an grid intersection
            patch_points = get_patch(
                (prev_x, prev_y, prev_dx, prev_dy), (x, y, dx, dy))

            patch = {}
            for pdx, pdy, px, py in patch_points:
                k = (px + prev_x, py + prev_y)
                if k in patch:
                    patch[k].append((pdx, pdy))
                else:
                    patch[k] = [(pdx, pdy)]

            assert(len(patch[(prev_x, prev_y)]) == 1)
            path[(z, prev_x, prev_y)][-1] += patch[(prev_x, prev_y)]

            for k, v in patch.items():
                assert(len(v) < 3)
                assert(len(v) > 0)
                if k != (prev_x, prev_y):
                    if (z, k[0], k[1]) in path:
                        path[(z, k[0], k[1])].append(v)
                    else:
                        path[(z, k[0], k[1])] = [v]

            assert((z, x, y) in path)
            path[(z, x, y)][-1].append((dx, dy))

        prev_x, prev_y, prev_dx, prev_dy = x, y, dx, dy


# eliminate duplicate points
for k in path.keys():
    filtered = []
    for poly in path[k]:
        prev = poly[0]
        new_poly = [prev]
        for p in poly[1:]:
            if prev != p:
                new_poly.append(p)
        filtered.append(new_poly)

    path[k] = filtered

name = os.path.splitext(os.path.basename(sys.argv[1]))[0]
draw_path(path, name)
save_zxy(path, name)
