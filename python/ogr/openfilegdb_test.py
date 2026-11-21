# Copyright 2015 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2014, Even Rouault <even dot rouault at spatialys dot com>
#
# SPDX-License-Identifier: MIT

"""Tests OGR handling of FileGDB datasets via the OpenFileGDB driver.

Format is described here:
  https://gdal.org/en/latest/drivers/vector/openfilegdb.html

Rewrite of:
  https://github.com/OSGeo/gdal/blob/c5a57bae66dcd36740ab9cfdf5459185824cb4af/autotest/ogr/ogr_openfilegdb.py
"""

from collections.abc import Iterator, Sequence
import contextlib
import math
import os
import pathlib
import shutil
import sys
import types
from typing import NamedTuple
import zipfile

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

_SINGLE_TO_MULTI_GEOM_TYPE: types.MappingProxyType[int, int] = (
    types.MappingProxyType({
        ogr.wkbLineString: ogr.wkbMultiLineString,
        ogr.wkbLineString25D: ogr.wkbMultiLineString25D,
        ogr.wkbPolygon: ogr.wkbMultiPolygon,
        ogr.wkbPolygon25D: ogr.wkbMultiPolygon25D,
        ogr.wkbLineStringM: ogr.wkbMultiLineStringM,
        ogr.wkbLineStringZM: ogr.wkbMultiLineStringZM,
        ogr.wkbPolygonM: ogr.wkbMultiPolygonM,
        ogr.wkbPolygonZM: ogr.wkbMultiPolygonZM,
    })
)

_SPI_IN_BUILDING = 0
_SPI_COMPLETED = 1
_SPI_INVALID = 2

_IDX_NOT_USED = 0
_IDX_USED = 1

_OPENFILEGDB_DATALIST: tuple[
    tuple[str, int, str | None] | tuple[str, int, str | None, str | None], ...
] = (
    ('none', ogr.wkbNone, None),
    ('point', ogr.wkbPoint, 'POINT (1 2)'),
    ('multipoint', ogr.wkbMultiPoint, 'MULTIPOINT (1 2,3 4)'),
    (
        'linestring',
        ogr.wkbLineString,
        'LINESTRING (1 2,3 4)',
        'MULTILINESTRING ((1 2,3 4))',
    ),
    ('multilinestring', ogr.wkbMultiLineString, 'MULTILINESTRING ((1 2,3 4))'),
    (
        'multilinestring_multipart',
        ogr.wkbMultiLineString,
        'MULTILINESTRING ((1 2,3 4),(5 6,7 8))',
    ),
    (
        'polygon',
        ogr.wkbPolygon,
        'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
        'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
    ),
    (
        'multipolygon',
        ogr.wkbMultiPolygon,
        (
            'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0),'
            '(0.25 0.25,0.75 0.25,0.75 0.75,0.25 0.75,0.25 0.25)),'
            '((2 0,2 1,3 1,3 0,2 0)))'
        ),
    ),
    ('point25D', ogr.wkbPoint25D, 'POINT (1 2 3)'),
    ('multipoint25D', ogr.wkbMultiPoint25D, 'MULTIPOINT (1 2 -10,3 4 -20)'),
    (
        'linestring25D',
        ogr.wkbLineString25D,
        'LINESTRING (1 2 -10,3 4 -20)',
        'MULTILINESTRING ((1 2 -10,3 4 -20))',
    ),
    (
        'multilinestring25D',
        ogr.wkbMultiLineString25D,
        'MULTILINESTRING ((1 2 -10,3 4 -20))',
    ),
    (
        'multilinestring25D_multipart',
        ogr.wkbMultiLineString25D,
        'MULTILINESTRING ((1 2 -10,3 4 -20),(5 6 -30,7 8 -40))',
    ),
    (
        'polygon25D',
        ogr.wkbPolygon25D,
        'POLYGON ((0 0 -10,0 1 -10,1 1 -10,1 0 -10,0 0 -10))',
        'MULTIPOLYGON (((0 0 -10,0 1 -10,1 1 -10,1 0 -10,0 0 -10)))',
    ),
    (
        'multipolygon25D',
        ogr.wkbMultiPolygon25D,
        'MULTIPOLYGON (((0 0 -10,0 1 -10,1 1 -10,1 0 -10,0 0 -10)))',
    ),
    (
        'multipatch',
        ogr.wkbGeometryCollection25D,
        (
            'GEOMETRYCOLLECTION Z ('
            'TIN Z (((0 0 0,0 1 0,1 0 0,0 0 0)),'
            '((0 1 0,1 0 0,1 1 0,0 1 0))),'
            'TIN Z (((10 0 0,10 1 0,11 0 0,10 0 0)),'
            '((10 0 0,11 0 0,10 -1 0,10 0 0))),'
            'TIN Z (((5 0 0,5 1 0,6 0 0,5 0 0))),'
            'MULTIPOLYGON Z (((100 0 0,100 1 0,101 1 0,101 0 0,100 0 0),'
            '(100.25 0.25 0,100.75 0.25 0,100.75 0.75 0,100.75 0.25 0,'
            '100.25 0.25 0))))'
        ),
    ),
    ('null_polygon', ogr.wkbPolygon, None),
    ('empty_polygon', ogr.wkbPolygon, 'POLYGON EMPTY', None),
    ('empty_multipoint', ogr.wkbMultiPoint, 'MULTIPOINT EMPTY', None),
)

_OPENFILEGDB_DATALIST_M: tuple[
    tuple[str, int, str] | tuple[str, int, str, str | None], ...
] = (
    ('pointm', ogr.wkbPointM, 'POINT M (1 2 3)'),
    ('pointzm', ogr.wkbPointZM, 'POINT ZM (1 2 3 4)'),
    ('multipointm', ogr.wkbMultiPointM, 'MULTIPOINT M ((1 2 3),(4 5 6))'),
    (
        'multipointzm',
        ogr.wkbMultiPointZM,
        'MULTIPOINT ZM ((1 2 3 4),(5 6 7 8))',
    ),
    (
        'linestringm',
        ogr.wkbLineStringM,
        'LINESTRING M (1 2 3,4 5 6)',
        'MULTILINESTRING M ((1 2 3,4 5 6))',
    ),
    (
        'linestringzm',
        ogr.wkbLineStringZM,
        'LINESTRING ZM (1 2 3 4,5 6 7 8)',
        'MULTILINESTRING ZM ((1 2 3 4,5 6 7 8))',
    ),
    (
        'multilinestringm',
        ogr.wkbMultiLineStringM,
        'MULTILINESTRING M ((1 2 3,4 5 6))',
    ),
    (
        'multilinestringzm',
        ogr.wkbMultiLineStringZM,
        'MULTILINESTRING ZM ((1 2 3 4,5 6 7 8))',
    ),
    (
        'polygonm',
        ogr.wkbPolygonM,
        'POLYGON M ((0 0 1,0 1 2,1 1 3,1 0 4,0 0 1))',
        'MULTIPOLYGON M (((0 0 1,0 1 2,1 1 3,1 0 4,0 0 1)))',
    ),
    (
        'polygonzm',
        ogr.wkbPolygonZM,
        'POLYGON ZM ((0 0 1 -1,0 1 2 -2,1 1 3 -3,1 0 4 -4,0 0 1 -1))',
        'MULTIPOLYGON ZM (((0 0 1 -1,0 1 2 -2,1 1 3 -3,1 0 4 -4,0 0 1 -1)))',
    ),
    (
        'multipolygonm',
        ogr.wkbMultiPolygonM,
        'MULTIPOLYGON M (((0 0 1,0 1 2,1 1 3,1 0 4,0 0 1)))',
    ),
    (
        'multipolygonzm',
        ogr.wkbMultiPolygonZM,
        'MULTIPOLYGON ZM (((0 0 1 -1,0 1 2 -2,1 1 3 -3,1 0 4 -4,0 0 1 -1)))',
    ),
    ('empty_polygonm', ogr.wkbPolygonM, 'POLYGON M EMPTY', None),
)

_POINT_ATTR_INDEX_CASES: tuple[tuple[str, tuple[int, ...], int], ...] = (
    ('id = 1', (1,), 2),
    ('1 = id', (1,), 2),
    ('id = 5', (5,), 2),
    ('id = 0', (), 2),
    ('id = 6', (), 2),
    ('id <= 1', (1,), 2),
    ('1 >= id', (1,), 2),
    ('id >= 5', (5,), 2),
    ('5 <= id', (5,), 2),
    ('id < 1', (), 2),
    ('1 > id', (), 2),
    ('id >= 1', (1, 2, 3, 4, 5), 2),
    ('id > 0', (1, 2, 3, 4, 5), 2),
    ('0 < id', (1, 2, 3, 4, 5), 2),
    ('id <= 5', (1, 2, 3, 4, 5), 2),
    ('id < 6', (1, 2, 3, 4, 5), 2),
    ('id <> 0', (1, 2, 3, 4, 5), 2),
    ('id IS NOT NULL', (1, 2, 3, 4, 5), 2),
    ('id IS NULL', (), 2),
    ('nullint IS NOT NULL', (), 2),
    ('nullint IS NULL', (1, 2, 3, 4, 5), 2),
    ("str = 'foo_e'", (), 1),
    ("str = 'foo_é'", (1, 2, 3, 4, 5), 1),
    ("str <= 'foo_é'", (1, 2, 3, 4, 5), 0),
    ("str >= 'foo_é'", (1, 2, 3, 4, 5), 1),
    ("str <> 'foo_é'", (), 0),
    ("str < 'foo_é'", (), 0),
    ("str > 'foo_é'", (), 0),
    ('smallint = -13', (1, 2, 3, 4, 5), 2),
    ('smallint <= -13', (1, 2, 3, 4, 5), 2),
    ('smallint >= -13', (1, 2, 3, 4, 5), 2),
    ('smallint < -13', (), 2),
    ('smallint > -13', (), 2),
    ('int = 123', (1, 2, 3, 4, 5), 2),
    ('int <= 123', (1, 2, 3, 4, 5), 2),
    ('int >= 123', (1, 2, 3, 4, 5), 2),
    ('int < 123', (), 2),
    ('int > 123', (), 2),
    ('float = 1.5', (1, 2, 3, 4, 5), 2),
    ('float <= 1.5', (1, 2, 3, 4, 5), 2),
    ('float >= 1.5', (1, 2, 3, 4, 5), 2),
    ('float < 1.5', (), 2),
    ('float > 1.5', (), 2),
    ('real = 4.56', (1, 2, 3, 4, 5), 2),
    ('real <= 4.56', (1, 2, 3, 4, 5), 2),
    ('real >= 4.56', (1, 2, 3, 4, 5), 2),
    ('real < 4.56', (), 2),
    ('real > 4.56', (), 2),
    ("adate = '2013/12/26 12:34:56'", (1, 2, 3, 4, 5), 2),
    ("adate <= '2013/12/26 12:34:56'", (1, 2, 3, 4, 5), 2),
    ("adate >= '2013/12/26 12:34:56'", (1, 2, 3, 4, 5), 2),
    ("adate < '2013/12/26 12:34:56'", (), 2),
    ("adate > '2013/12/26 12:34:56'", (), 2),
    ("guid = '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", (1, 2, 3, 4, 5), 2),
    ("guid <= '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", (1, 2, 3, 4, 5), 2),
    ("guid >= '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", (1, 2, 3, 4, 5), 2),
    ("guid < '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", (), 2),
    ("guid > '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", (), 2),
    ("guid = '{'", (), 2),
    ("guid > '{'", (1, 2, 3, 4, 5), 2),
    ('NOT(id = 1)', (2, 3, 4, 5), 2),
    ('id = 1 OR id = -1', (1,), 2),
    ('id = -1 OR id = 1', (1,), 2),
    ('id = 1 OR id = 1', (1,), 2),
    # Exclusive OR branches.
    ('id = 1 OR id = 2', (1, 2), 2),
    ('id < 3 OR id > 3', (1, 2, 4, 5), 2),
    ('id > 3 OR id < 3', (1, 2, 4, 5), 2),
    ('id <= 3 OR id >= 4', (1, 2, 3, 4, 5), 2),
    ('id >= 4 OR id <= 3', (1, 2, 3, 4, 5), 2),
    ('id < 3 OR id >= 3', (1, 2, 3, 4, 5), 2),
    ('id <= 3 OR id >= 3', (1, 2, 3, 4, 5), 2),
    ('id <= 5 OR id >= 1', (1, 2, 3, 4, 5), 2),
    ('id <= 1.5 OR id >= 2', (1, 2, 3, 4, 5), 2),
    ('id IS NULL OR id IS NOT NULL', (1, 2, 3, 4, 5), 2),
    ('float < 1.5 OR float > 1.5', (), 2),
    ('float <= 1.5 OR float >= 1.5', (1, 2, 3, 4, 5), 2),
    ('float < 1.5 OR float > 2', (), 2),
    ('float < 1 OR float > 2.5', (), 2),
    ("str < 'foo_é' OR str > 'z'", (), 0),
    ("adate < '2013/12/26 12:34:56' OR adate > '2014/01/01'", (), 2),
    ('id = 1 AND id = -1', (), 2),
    ('id = -1 AND id = 1', (), 2),
    ('id = 1 AND id = 1', (1,), 2),
    ('id = 1 AND id = 2', (), 2),
    ('id <= 5 AND id >= 1', (1, 2, 3, 4, 5), 2),
    ('id <= 3 AND id >= 3', (3,), 2),
    ('id = 1 AND float = 1.5', (1,), 2),
    ('id BETWEEN 1 AND 5', (1, 2, 3, 4, 5), 2),
    ('id IN (1)', (1,), 2),
    ('id IN (5,4,3,2,1)', (1, 2, 3, 4, 5), 2),
    # Expressions that cannot use field indices (or only partially).
    ('fid = 1', (1,), 0),
    ('fid BETWEEN 1 AND 1', (1,), 0),
    ('fid IN (1)', (1,), 0),
    ('fid IS NULL', (), 0),
    ('fid IS NOT NULL', (1, 2, 3, 4, 5), 0),
    ("xml <> ''", (1, 2, 3, 4, 5), 0),
    ("id = 1 AND xml <> ''", (1,), 1),
    ("xml <> '' AND id = 1", (1,), 1),
    ("NOT(id = 1 AND xml <> '')", (2, 3, 4, 5), 0),
    ("id = 1 OR xml <> ''", (1, 2, 3, 4, 5), 0),
    ('id = id', (1, 2, 3, 4, 5), 0),
    ('id = 1 + 0', (1,), 0),
)

_NONE_ATTR_INDEX_CASES: tuple[tuple[str, tuple[int, ...], int], ...] = (
    ('id = 1', (1,), 2),
    ('id IS NULL', (6,), 2),
    ('id IS NOT NULL', (1, 2, 3, 4, 5), 2),
    ('id IS NULL OR id IS NOT NULL', (1, 2, 3, 4, 5, 6), 2),
    ('id = 1 OR id IS NULL', (1, 6), 2),
    ('id IS NULL OR id = 1', (1, 6), 2),
)

_BIG_LAYER_ATTR_INDEX_CASES: tuple[tuple[str, int, int | None], ...] = (
    ('real = 0', 86, 1),
    ('real = 1', 85, 2),
    ('real = 2', 85, 3),
    ('real = 3', 85, 4),
    ('real >= 0', 86 + 3 * 85, None),
    ('real < 4', 86 + 3 * 85, None),
    ('real > 1 AND real < 2', 0, None),
    ('real < 0', 0, None),
)

_STR_INDEXED_TRUNCATED_CASES: tuple[
    tuple[str, str, tuple[int, ...], int], ...
] = (
    ('eq_a', "str = 'a'", (1,), _IDX_USED),
    ('eq_aa', "str = 'aa'", (2,), _IDX_USED),
    ('ne_aa', "str != 'aa'", (1, 3), _IDX_NOT_USED),
    ('eq_aaa', "str = 'aaa'", (3,), _IDX_USED),
    ('ge_aaa', "str >= 'aaa'", (3,), _IDX_USED),
    ('gt_aaa', "str > 'aaa'", (), _IDX_NOT_USED),
    ('gt_aa_underscore', "str > 'aa_'", (3,), _IDX_NOT_USED),
    ('le_aab', "str <= 'aab'", (1, 2, 3), _IDX_NOT_USED),
    ('eq_aaa_space', "str = 'aaa '", (), _IDX_USED),
    ('ne_aaa_space', "str != 'aaa '", (1, 2, 3), _IDX_NOT_USED),
    ('le_aaa_space', "str <= 'aaa '", (1, 2, 3), _IDX_NOT_USED),
    ('le_aaaX', "str <= 'aaaX'", (1, 2, 3), _IDX_NOT_USED),
    ('ge_aaa_space', "str >= 'aaa '", (), _IDX_USED),
    ('eq_aaaX', "str = 'aaaX'", (), _IDX_USED),
    ('eq_aaaXX', "str = 'aaaXX'", (), _IDX_USED),
    ('eq_aaa_two_spaces', "str = 'aaa  '", (), _IDX_USED),
    ('in_a_b', "str IN ('a', 'b')", (1,), _IDX_USED),
    ('in_aaa', "str IN ('aaa')", (3,), _IDX_USED),
    ('in_aaa_and_space', "str IN ('aaa', 'aaa ')", (3,), _IDX_USED),
    ('in_aaa_space', "str IN ('aaa ')", (), _IDX_USED),
    ('in_aaaX', "str IN ('aaaX')", (), _IDX_USED),
    ('in_aaaXX', "str IN ('aaaXX')", (), _IDX_USED),
    ('ilike_a', "str ILIKE 'a'", (1,), _IDX_NOT_USED),
    ('ilike_a_pct', "str ILIKE 'a%'", (1, 2, 3), _IDX_NOT_USED),
    ('ilike_aaa_two_spaces', "str ILIKE 'aaa  '", (), _IDX_NOT_USED),
)


class _OrderByOptimizedCase(NamedTuple):
  name: str
  sql: str
  feat_count: int
  first_fid: int | None
  sort_col: str
  descending: bool


_ORDER_BY_OPTIMIZED_CASES: tuple[_OrderByOptimizedCase, ...] = (
    _OrderByOptimizedCase(
        'point_asc', 'select * from point order by id', 5, 1, 'id', False
    ),
    _OrderByOptimizedCase(
        'point_desc',
        'select id, str from point order by id desc',
        5,
        5,
        'id',
        True,
    ),
    _OrderByOptimizedCase(
        'point_where_id_1',
        'select * from point where id = 1 order by id',
        1,
        1,
        'id',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_asc',
        'select * from big_layer order by real',
        86 + 3 * 85,
        1,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_0',
        'select * from big_layer order by real limit 0',
        0,
        None,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_offset_10000',
        'select * from big_layer order by real offset 10000',
        0,
        None,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_1',
        'select * from big_layer order by real limit 1',
        1,
        1,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_1_offset_0',
        'select * from big_layer order by real limit 1 offset 0',
        1,
        1,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_1_offset_1',
        'select * from big_layer order by real limit 1 offset 1',
        1,
        5,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_2',
        'select * from big_layer order by real limit 2',
        2,
        1,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_100000',
        'select * from big_layer order by real limit 100000',
        86 + 3 * 85,
        1,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_limit_100000_offset_1',
        'select * from big_layer order by real limit 100000 offset 1',
        86 + 3 * 85 - 1,
        5,
        'real',
        False,
    ),
    _OrderByOptimizedCase(
        'big_layer_desc',
        'select * from big_layer order by real desc',
        86 + 3 * 85,
        4 * 85,
        'real',
        True,
    ),
)

_ORDER_BY_NON_OPTIMIZED_CASES: tuple[tuple[str, str, int], ...] = (
    ('nonopt_xml', 'select * from point order by xml', 5),
    ('nonopt_fid_col', 'select fid from point order by id', 5),
    ('nonopt_cast', 'select cast(id as float) from point order by id', 5),
    ('nonopt_distinct', 'select distinct id from point order by id', 5),
    ('nonopt_const', 'select 1 from point order by id', 5),
    ('nonopt_count', 'select count(*) from point order by id', 1),
    ('nonopt_nullint', 'select * from point order by nullint', 5),
    (
        'nonopt_where_or',
        'select * from point where id = 1 or id = 2 order by id',
        2,
    ),
    (
        'nonopt_multi_order',
        'select * from point where id = 1 order by id, float',
        1,
    ),
    (
        'nonopt_where_other',
        'select * from point where float > 0 order by id',
        5,
    ),
)

_ORDER_BY_INVALID_CASES: tuple[tuple[str, str, str], ...] = (
    ('missing_table', 'select foo from', r'SQL Expression Parsing Error:'),
    (
        'unknown_table',
        'select foo from bar',
        r'SELECT from table bar failed, no such table/featureclass',
    ),
    (
        'unknown_order_col',
        'select * from point order by foo',
        r'Unrecognized field name foo in ORDER BY',
    ),
)

_EXPECTED_HOLE_STRS: tuple[str | None, ...] = (
    'fid13',
    'fid2',
    'fid3',
    'fid4',
    'fid5',
    'fid6',
    'fid7',
    'fid8',
    'fid9',
    'fid10',
    'fid11',
    None,
)

_CORRUPT_OFFSETS: tuple[tuple[str, tuple[int, ...], frozenset[int]], ...] = (
    (
        'a00000001.gdbtable',
        (
            4,
            5,
            6,
            7,
            32,
            33,
            41,
            42,
            52,
            59,
            60,
            63,
            64,
            72,
            73,
            77,
            78,
            79,
            80,
            81,
            101,
            102,
            104,
            105,
            111,
            180,
        ),
        frozenset(),
    ),
    (
        'a00000001.gdbtablx',
        (4, 7, 11, 12, 16, 31, 5136, 5140, 5142, 5144),
        frozenset(),
    ),
    (
        'a00000004.gdbindexes',
        (
            0,
            4,
            5,
            44,
            45,
            66,
            67,
            100,
            101,
            116,
            117,
            148,
            149,
            162,
            163,
            206,
            207,
            220,
            221,
            224,
            280,
            281,
        ),
        frozenset(),
    ),
    (
        'a00000004.CatItemsByPhysicalName.atx',
        (4, 12, 8196, 8300, 8460, 8620, 8780, 8940, 9100, 12290, 12294, 12298),
        frozenset((8300, 8460, 8620, 8780, 8940, 9100)),
    ),
)

_FIELD_ALIASES: tuple[tuple[str, str], ...] = (
    ('text', 'My Text Field'),
    ('short_int', 'My Short Int Field'),
    ('long_int', 'My Long Int Field'),
    ('float', 'My Float Field'),
    ('double', 'My Double Field'),
    ('date', 'My Date Field'),
    ('blob', 'My Blob Field'),
    ('guid', 'My GUID field'),
    ('raster', 'My Raster Field'),
    ('SHAPE_Length', ''),
)

_V10_SYSTEM_TABLES: tuple[str, ...] = (
    'GDB_DBTune',
    'GDB_ItemRelationshipTypes',
    'GDB_ItemRelationships',
    'GDB_ItemTypes',
    'GDB_Items',
    'GDB_SpatialRefs',
    'GDB_SystemCatalog',
)

_V9_SYSTEM_TABLES: tuple[str, ...] = (
    'GDB_DBTune',
    'GDB_FeatureClasses',
    'GDB_FeatureDataset',
    'GDB_FieldInfo',
    'GDB_ObjectClasses',
    'GDB_Release',
    'GDB_SpatialRefs',
    'GDB_SystemCatalog',
    'GDB_UserMetadata',
)


@contextlib.contextmanager
def _attribute_filter(layer: ogr.Layer, expr: str | None) -> Iterator[None]:
  layer.SetAttributeFilter(expr)
  try:
    yield
  finally:
    layer.SetAttributeFilter(None)


@contextlib.contextmanager
def _spatial_filter_rect(
    layer: ogr.Layer, minx: float, miny: float, maxx: float, maxy: float
) -> Iterator[None]:
  layer.SetSpatialFilterRect(minx, miny, maxx, maxy)
  try:
    yield
  finally:
    layer.SetSpatialFilter(None)


@contextlib.contextmanager
def _capture_errors() -> Iterator[list[tuple[int, int, str]]]:
  records: list[tuple[int, int, str]] = []

  def handler(err_class: int, err_no: int, msg: str) -> None:
    records.append((err_class, err_no, msg))

  gdal.PushErrorHandler(handler)
  try:
    yield records
  finally:
    gdal.PopErrorHandler()


class OpenFileGDBTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    for mod in (gdal, ogr, osr):
      self.enter_context(mod.ExceptionMgr())
    self.driver = gdal.GetDriverByName('OpenFileGDB')
    self.assertIsNotNone(self.driver)

  def _test_file_path(self, rel_path: str) -> str:
    filepath = ogr_util.GetTestFilePath(rel_path)
    self.assertTrue(
        pathlib.Path(filepath).exists(), f'Missing test fixture: {filepath}'
    )
    return filepath

  def _open_gdb(
      self,
      path: str | os.PathLike[str],
      *,
      update: bool = False,
      open_options: Sequence[str] | None = None,
  ) -> gdal.Dataset:
    flags = gdal.OF_VECTOR | (gdal.OF_UPDATE if update else gdal.OF_READONLY)
    return gdal.OpenEx(
        str(path),
        flags,
        allowed_drivers=['OpenFileGDB'],
        open_options=list(open_options or ()),
    )

  def _query_int(self, ds: gdal.Dataset, sql: str) -> int:
    with ds.ExecuteSQL(sql) as sql_lyr:
      self.assertIsNotNone(sql_lyr)
      feat = sql_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      return int(feat.GetField(0))

  def _assert_geometry_equal(
      self,
      actual: ogr.Geometry | None,
      expected: ogr.Geometry | str | None,
      # Default 1e-4 tolerates curve densification, CSV decimal rounding, and
      # x86_64 vs Arm libm trigonometric variance.
      max_error: float = 1e-4,
  ) -> None:
    expected_geom = (
        ogr.CreateGeometryFromWkt(expected)
        if isinstance(expected, str)
        else expected
    )
    if expected_geom is None:
      self.assertIsNone(actual)
      return
    self.assertIsNotNone(actual)
    assert actual is not None
    self.assertEqual(actual.GetGeometryType(), expected_geom.GetGeometryType())
    self.assertEqual(bool(actual.Is3D()), bool(expected_geom.Is3D()))
    self.assertEqual(
        bool(actual.IsMeasured()), bool(expected_geom.IsMeasured())
    )
    self.assertEqual(
        actual.CoordinateDimension(), expected_geom.CoordinateDimension()
    )
    self.assertEqual(bool(actual.IsEmpty()), bool(expected_geom.IsEmpty()))
    self.assertEqual(
        actual.GetGeometryCount(), expected_geom.GetGeometryCount()
    )
    self.assertEqual(actual.GetPointCount(), expected_geom.GetPointCount())
    if actual.GetGeometryCount() > 0:
      for idx in range(actual.GetGeometryCount()):
        self._assert_geometry_equal(
            actual.GetGeometryRef(idx),
            expected_geom.GetGeometryRef(idx),
            max_error=max_error,
        )
    else:
      for idx in range(actual.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(idx), expected_geom.GetX(idx), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetY(idx), expected_geom.GetY(idx), delta=max_error
        )
        if expected_geom.Is3D():
          self.assertAlmostEqual(
              actual.GetZ(idx), expected_geom.GetZ(idx), delta=max_error
          )
        if expected_geom.IsMeasured():
          actual_m = actual.GetM(idx)
          expected_m = expected_geom.GetM(idx)
          if math.isnan(expected_m):
            self.assertTrue(math.isnan(actual_m))
          else:
            self.assertAlmostEqual(actual_m, expected_m, delta=max_error)

  def _unzip_testopenfilegdb(self, dest_dir: pathlib.Path) -> pathlib.Path:
    zip_path = self._test_file_path('testopenfilegdb.gdb.zip')
    with zipfile.ZipFile(zip_path) as zf:
      zf.extractall(dest_dir)
    gdb_dir = dest_dir / 'testopenfilegdb.gdb'
    self.assertTrue(gdb_dir.is_dir())
    return gdb_dir

  def test_read_point(self) -> None:
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      self.assertEqual(ds.GetDriver().ShortName, 'OpenFileGDB')
      self.assertEqual(ds.GetLayerCount(), 37)
      layer = ds.GetLayerByName('point')
      self.assertEqual(layer.GetName(), 'point')
      self.assertEqual(layer.GetFeatureCount(), 5)
      self.assertEqual(layer.GetExtent(), (1.0, 1.0, 2.0, 2.0))

      layer_defn = layer.GetLayerDefn()
      self.assertEqual(layer_defn.GetFieldCount(), 12)
      self.assertEqual(layer_defn.GetGeomType(), ogr.wkbPoint)

      field_defn = layer_defn.GetFieldDefn(0)
      self.assertEqual(field_defn.GetName(), 'id')
      self.assertEqual(field_defn.GetTypeName(), 'Integer')

      field_defn = layer_defn.GetFieldDefn(7)
      self.assertEqual(field_defn.GetName(), 'guid')
      self.assertEqual(field_defn.GetTypeName(), 'String')

      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetField(0), 1)
      self.assertEqual(feature.GetFieldCount(), 12)
      self.assertEqual(feature.GetGeomFieldCount(), 1)
      geometry_ref = feature.GetGeometryRef()
      self.assertIsNotNone(geometry_ref)
      self._assert_geometry_equal(geometry_ref, 'POINT (1 2)', max_error=1e-9)
      px, py, pz = geometry_ref.GetPoint()
      self.assertAlmostEqual(px, 1.0, delta=1e-9)
      self.assertAlmostEqual(py, 2.0, delta=1e-9)
      self.assertEqual(pz, 0.0)
      px2, py2 = geometry_ref.GetPoint_2D()
      self.assertAlmostEqual(px2, 1.0, delta=1e-9)
      self.assertAlmostEqual(py2, 2.0, delta=1e-9)

  # Upstream: test_ogr_openfilegdb_1 (and test_ogr_openfilegdb_2 omitted:
  # test_ogrsf binary is excluded from Google3's //third_party/gdal).
  @parameterized.named_parameters(
      dict(
          testcase_name='v10',
          filename='testopenfilegdb.gdb.zip',
          version10=True,
      ),
      dict(
          testcase_name='v92',
          filename='testopenfilegdb92.gdb.zip',
          version10=False,
      ),
      dict(
          testcase_name='v93',
          filename='testopenfilegdb93.gdb.zip',
          version10=False,
      ),
  )
  def test_basic_datasets(self, filename: str, version10: bool) -> None:
    filepath = self._test_file_path(filename)
    srs = osr.SpatialReference()
    srs.SetFromUserInput('WGS84')

    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, r'not recognized'),
    ):
      with gdal.OpenEx(filepath, gdal.OF_RASTER) as _:
        pass

    with gdal.OpenEx(filepath, gdal.OF_RASTER | gdal.OF_VECTOR) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.GetDriver().ShortName, 'OpenFileGDB')
      self.assertEqual(ds.GetLayerCount(), 37 if version10 else 22)

    with self._open_gdb(filepath) as ds:
      for data in _OPENFILEGDB_DATALIST:
        lyr_name = data[0]
        if (
            lyr_name in (
                'multilinestring_multipart',
                'multilinestring25D_multipart',
            )
            and not version10
        ):
          continue
        with self.subTest(layer=lyr_name):
          layer = ds.GetLayerByName(lyr_name)
          self.assertIsNotNone(layer)
          self.assertEqual(
              layer.GetDataset().GetDescription(), ds.GetDescription()
          )
          expected_geom_type = _SINGLE_TO_MULTI_GEOM_TYPE.get(data[1], data[1])

          self.assertEqual(layer.GetGeomType(), expected_geom_type)
          layer_defn = layer.GetLayerDefn()
          if expected_geom_type != ogr.wkbNone:
            geom_field_defn = layer_defn.GetGeomFieldDefn(0)
            self.assertEqual(geom_field_defn.type, expected_geom_type)
            self.assertEqual(geom_field_defn.IsNullable(), 1)
          str_defn = layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('str'))
          self.assertEqual(str_defn.GetWidth(), 0)
          smallint_defn = layer_defn.GetFieldDefn(
              layer_defn.GetFieldIndex('smallint')
          )
          self.assertEqual(smallint_defn.GetType(), ogr.OFTInteger)
          self.assertEqual(smallint_defn.GetSubType(), ogr.OFSTInt16)
          float_defn = layer_defn.GetFieldDefn(
              layer_defn.GetFieldIndex('float')
          )
          self.assertEqual(float_defn.GetType(), ogr.OFTReal)
          self.assertEqual(float_defn.GetSubType(), ogr.OFSTFloat32)
          if data[1] != ogr.wkbNone:
            self.assertIsSame(
                layer.GetSpatialRef(),
                srs,
                options=['IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES'],
            )

          feat = layer.GetNextFeature()
          self.assertIsNotNone(feat)
          if data[1] != ogr.wkbNone:
            expected_wkt = data[3] if len(data) > 3 else data[2]
            self._assert_geometry_equal(feat.GetGeometryRef(), expected_wkt)

          self.assertEqual(feat.GetField('id'), 1)
          self.assertEqual(feat.GetField('smallint'), -13)
          self.assertEqual(feat.GetField('int'), 123)
          self.assertEqual(feat.GetField('float'), 1.5)
          self.assertEqual(feat.GetField('real'), 4.56)
          self.assertEqual(feat.GetField('adate'), '2013/12/26 12:34:56')
          self.assertEqual(
              feat.GetField('guid'),
              '{12345678-9ABC-DEF0-1234-567890ABCDEF}',
          )
          if version10:
            self.assertEqual(feat.GetField('xml'), '<foo></foo>')
          self.assertEqual(feat.GetField('binary'), '00FF7F')
          self.assertEqual(feat.GetField('binary2'), '123456')

          if version10:
            with ds.ExecuteSQL(
                f'GetLayerDefinition {layer.GetName()}'
            ) as sql_lyr:
              self.assertIsNotNone(sql_lyr)
              def_feat = sql_lyr.GetNextFeature()
              self.assertIsNotNone(def_feat)
              xml_def = def_feat.GetField(0)
              self.assertIn(layer.GetName(), xml_def)
              self.assertRegex(xml_def, r'<typens:DE(?:FeatureClass|Table)Info')
              self.assertIsNone(sql_lyr.GetNextFeature())
            layer.ResetReading()
            self.assertFalse(layer.TestCapability('foo'))

            with ds.ExecuteSQL(
                f'GetLayerMetadata {layer.GetName()}'
            ) as sql_lyr:
              self.assertIsNotNone(sql_lyr)
              self.assertIsNotNone(sql_lyr.GetNextFeature())
              self.assertIsNone(sql_lyr.GetNextFeature())

      if version10:
        for sql_cmd in ('GetLayerDefinition foo', 'GetLayerMetadata foo'):
          with (
              self.subTest(sql=sql_cmd),
              gdal.quiet_errors(),
              self.assertRaisesRegex(RuntimeError, r'Invalid layer name: foo'),
          ):
            with ds.ExecuteSQL(sql_cmd) as _:
              pass

        for data_m in _OPENFILEGDB_DATALIST_M:
          with self.subTest(layer=data_m[0]):
            layer = ds.GetLayerByName(data_m[0])
            expected_geom_type = _SINGLE_TO_MULTI_GEOM_TYPE.get(
                data_m[1], data_m[1]
            )
            self.assertEqual(layer.GetGeomType(), expected_geom_type)
            feat = layer.GetNextFeature()
            self.assertIsNotNone(feat)
            expected_wkt = data_m[3] if len(data_m) > 3 else data_m[2]
            self._assert_geometry_equal(feat.GetGeometryRef(), expected_wkt)

  def test_open_table_directly(self) -> None:
    # Upstream: test_ogr_openfilegdb_3
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(
        f'/vsizip/{filepath}/testopenfilegdb.gdb/a00000009.gdbtable'
    ) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetName(), 'none')

      # Try opening a system table directly by name.
      layer = ds.GetLayerByName('GDB_SystemCatalog')
      self.assertEqual(layer.GetName(), 'GDB_SystemCatalog')
      feat = layer.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('Name'), 'GDB_SystemCatalog')
      layer = ds.GetLayerByName('GDB_SystemCatalog')
      self.assertEqual(layer.GetName(), 'GDB_SystemCatalog')

  def _assert_attribute_index_cases(
      self,
      ds: gdal.Dataset,
      layer_name: str,
      cases: tuple[tuple[str, tuple[int, ...], int], ...],
  ) -> None:
    layer = ds.GetLayerByName(layer_name)
    self.assertIsNotNone(layer)
    for where_clause, expected_fids, expected_attr_index_use in cases:
      with self.subTest(layer=layer_name, where=where_clause):
        with _attribute_filter(layer, where_clause):
          attr_index_use = self._query_int(
              ds, f'GetLayerAttrIndexUse {layer.GetName()}'
          )
          self.assertEqual(attr_index_use, expected_attr_index_use)
          expected_count = len(expected_fids)
          self.assertEqual(layer.GetFeatureCount(), expected_count)
          actual_fids = tuple(feat.GetFID() for feat in layer)
          self.assertEqual(actual_fids, expected_fids)

  def test_attribute_indexes(self) -> None:
    # Upstream: test_ogr_openfilegdb_4
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(f'/vsizip/{filepath}/testopenfilegdb.gdb') as ds:
      self._assert_attribute_index_cases(ds, 'point', _POINT_ATTR_INDEX_CASES)
      self._assert_attribute_index_cases(ds, 'none', _NONE_ATTR_INDEX_CASES)

      layer = ds.GetLayerByName('big_layer')
      for where_clause, count, start in _BIG_LAYER_ATTR_INDEX_CASES:
        with self.subTest(layer='big_layer', where=where_clause):
          with _attribute_filter(layer, where_clause):
            self.assertEqual(layer.GetFeatureCount(), count)
            for i in range(count):
              feat = layer.GetNextFeature()
              self.assertIsNotNone(feat)
              if start is not None:
                self.assertEqual(feat.GetFID(), i * 4 + start)
            self.assertIsNone(layer.GetNextFeature())

  @parameterized.named_parameters(
      dict(
          testcase_name=name,
          where_clause=where_clause,
          expected_fids=fids,
          expected_attr_index_use=expected_attr_index_use,
      )
      for name, where_clause, fids, expected_attr_index_use in (
          _STR_INDEXED_TRUNCATED_CASES
      )
  )
  def test_str_indexed_truncated(
      self,
      where_clause: str,
      expected_fids: tuple[int, ...],
      expected_attr_index_use: int,
  ) -> None:
    # Upstream: test_ogr_openfilegdb_str_indexed_truncated
    filepath = self._test_file_path('fgdb/test_str_indexed_truncated.gdb')
    with self._open_gdb(filepath) as ds:
      layer = ds.GetLayerByName('test')
      with _attribute_filter(layer, where_clause):
        attr_index_use = self._query_int(
            ds, f'GetLayerAttrIndexUse {layer.GetName()}'
        )
        self.assertEqual(attr_index_use, expected_attr_index_use)
        self.assertEqual(tuple(f.GetFID() for f in layer), expected_fids)

  def test_ilike(self) -> None:
    # Upstream: test_ogr_openfilegdb_ilike
    filepath = self._test_file_path('fgdb/Domains.gdb/a00000001.gdbtable')
    with self._open_gdb(filepath) as ds:
      layer = ds.GetLayer(0)
      with _attribute_filter(layer, "Name = 'Roads'"):
        self.assertEqual(layer.GetFeatureCount(), 1)
      with _attribute_filter(layer, "Name ILIKE 'Roads'"):
        self.assertEqual(layer.GetFeatureCount(), 1)
      with _attribute_filter(layer, "Name = 'Roadsx'"):
        self.assertEqual(layer.GetFeatureCount(), 0)
      with _attribute_filter(layer, "Name ILIKE 'Roadsx'"):
        self.assertEqual(layer.GetFeatureCount(), 0)

  def test_open_unzipped_dataset(self) -> None:
    # Upstream: test_ogr_openfilegdb_5
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    gdb_dir = self._unzip_testopenfilegdb(tmp_dir)
    with self._open_gdb(gdb_dir) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.GetLayerCount(), 37)
      self.assertEqual(ds.GetLayerByName('point').GetFeatureCount(), 5)

  def test_sql_aggregates(self) -> None:
    # Upstream: test_ogr_openfilegdb_6
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      # With indices.
      with ds.ExecuteSQL(
          'select min(id), max(id), count(id), sum(id), avg(id), min(str), '
          'min(smallint), avg(smallint), min(float), avg(float), '
          'min(real), avg(real), min(adate), avg(adate), min(guid), '
          'min(nullint), avg(nullint) from point'
      ) as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('MIN_id'), 1)
        self.assertEqual(feat.GetField('MAX_id'), 5)
        self.assertEqual(feat.GetField('COUNT_id'), 5)
        self.assertEqual(feat.GetField('SUM_id'), 15.0)
        self.assertEqual(feat.GetField('AVG_id'), 3.0)
        self.assertEqual(feat.GetField('MIN_str')[0:4], 'foo_')
        self.assertEqual(feat.GetField('MIN_smallint'), -13)
        self.assertEqual(feat.GetField('AVG_smallint'), -13)
        self.assertEqual(feat.GetField('MIN_float'), 1.5)
        self.assertEqual(feat.GetField('AVG_float'), 1.5)
        self.assertEqual(feat.GetField('MIN_real'), 4.56)
        self.assertEqual(feat.GetField('AVG_real'), 4.56)
        self.assertEqual(feat.GetField('MIN_adate'), '2013/12/26 12:34:56')
        self.assertEqual(feat.GetField('AVG_adate'), '2013/12/26 12:34:56')
        self.assertEqual(
            feat.GetField('MIN_guid'),
            '{12345678-9ABC-DEF0-1234-567890ABCDEF}',
        )
        self.assertFalse(feat.IsFieldSet('MIN_nullint'))
        self.assertFalse(feat.IsFieldSet('AVG_nullint'))

      # Without index.
      with ds.ExecuteSQL('select min(id), avg(id) from multipoint') as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('MIN_id'), 1)
        self.assertEqual(feat.GetField('AVG_id'), 3.0)

  @parameterized.named_parameters(
      dict(
          testcase_name=case.name,
          sql=case.sql,
          feat_count=case.feat_count,
          first_fid=case.first_fid,
          sort_col=case.sort_col,
          descending=case.descending,
      )
      for case in _ORDER_BY_OPTIMIZED_CASES
  )
  def test_sql_order_by_optimized(
      self,
      sql: str,
      feat_count: int,
      first_fid: int | None,
      sort_col: str,
      descending: bool,
  ) -> None:
    # Upstream: test_ogr_openfilegdb_7 (optimized queries; test_ogrsf checks
    # omitted since test_ogrsf is excluded from Google3's //third_party/gdal).
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      with ds.ExecuteSQL(sql) as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        self.assertEqual(sql_lyr.GetFeatureCount(), feat_count)
        fids: list[int] = []
        sort_keys: list[float] = []
        for feat in sql_lyr:
          fids.append(feat.GetFID())
          sort_keys.append(feat.GetField(sort_col))
        self.assertLen(fids, feat_count)
        if first_fid is not None:
          self.assertEqual(fids[0], first_fid)
        for prev_key, next_key in zip(sort_keys, sort_keys[1:]):
          if descending:
            self.assertGreaterEqual(prev_key, next_key)
          else:
            self.assertLessEqual(prev_key, next_key)

      optimized = self._query_int(ds, 'GetLastSQLUsedOptimizedImplementation')
      self.assertEqual(optimized, 1)

  @parameterized.named_parameters(
      dict(testcase_name=name, sql=sql, feat_count=feat_count)
      for name, sql, feat_count in _ORDER_BY_NON_OPTIMIZED_CASES
  )
  def test_sql_order_by_non_optimized(self, sql: str, feat_count: int) -> None:
    # Upstream: test_ogr_openfilegdb_7 (non-optimized queries)
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      with ds.ExecuteSQL(sql) as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        self.assertEqual(sql_lyr.GetFeatureCount(), feat_count)

      optimized = self._query_int(ds, 'GetLastSQLUsedOptimizedImplementation')
      self.assertEqual(optimized, 0)

  @parameterized.named_parameters(
      dict(testcase_name=name, sql=sql, error_regex=error_regex)
      for name, sql, error_regex in _ORDER_BY_INVALID_CASES
  )
  def test_sql_order_by_invalid(self, sql: str, error_regex: str) -> None:
    # Upstream: test_ogr_openfilegdb_7 (invalid queries)
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, error_regex),
      ):
        with ds.ExecuteSQL(sql) as _:
          pass

  def test_ignore_gdbtablx(self) -> None:
    # Upstream: test_ogr_openfilegdb_8
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      counts_default = {
          ds.GetLayer(i).GetName(): ds.GetLayer(i).GetFeatureCount()
          for i in range(ds.GetLayerCount())
      }

    with gdal.config_option('OPENFILEGDB_IGNORE_GDBTABLX', 'YES'):
      with self._open_gdb(filepath) as ds:
        counts_ignored = {
            ds.GetLayer(i).GetName(): ds.GetLayer(i).GetFeatureCount()
            for i in range(ds.GetLayerCount())
        }
        self.assertEqual(counts_default, counts_ignored)

        hole_lyr = ds.GetLayerByName('hole')
        actual_strs = tuple(feat.GetField('str') for feat in hole_lyr)
        self.assertEqual(actual_strs, _EXPECTED_HOLE_STRS)

  def test_open_gdbtable_outside_gdb(self) -> None:
    # Upstream: test_ogr_openfilegdb_9
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    gdb_dir = self._unzip_testopenfilegdb(tmp_dir / 'unzip')
    standalone_dir = tmp_dir / 'standalone'
    standalone_dir.mkdir()
    shutil.copy(
        gdb_dir / 'a00000009.gdbtable', standalone_dir / 'a00000009.gdbtable'
    )
    shutil.copy(
        gdb_dir / 'a00000009.gdbtablx', standalone_dir / 'a00000009.gdbtablx'
    )

    with self._open_gdb(standalone_dir / 'a00000009.gdbtable') as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetName(), 'a00000009')
      feat = layer.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('id'), 1)

  def test_corrupted_files(self) -> None:
    # Upstream: test_ogr_openfilegdb_10
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    gdb_dir = self._unzip_testopenfilegdb(tmp_dir)

    for rel_file, offsets, silent_offsets in _CORRUPT_OFFSETS:
      target_file = gdb_dir / rel_file
      self.assertTrue(target_file.is_file())
      orig_data = target_file.read_bytes()
      use_gdb_items = rel_file.startswith('a00000004.')
      layer_name = 'GDB_Items' if use_gdb_items else 'GDB_SystemCatalog'

      for offset in offsets:
        with self.subTest(file=rel_file, offset=offset):
          corrupted = bytearray(orig_data)
          corrupted[offset] = 255 - corrupted[offset]
          try:
            target_file.write_bytes(bytes(corrupted))
            with (
                gdal.ExceptionMgr(useExceptions=False),
                ogr.ExceptionMgr(useExceptions=False),
                _capture_errors() as records,
            ):
              gdal.ErrorReset()
              ds = gdal.OpenEx(
                  str(gdb_dir),
                  gdal.OF_VECTOR | gdal.OF_READONLY,
                  allowed_drivers=['OpenFileGDB'],
              )
              try:
                feat = None
                if ds is not None:
                  lyr = ds.GetLayerByName(layer_name)
                  if lyr is not None:
                    if use_gdb_items:
                      lyr.SetAttributeFilter("PhysicalName = 'NO_FIELD'")
                    feat = lyr.GetNextFeature()
              finally:
                if ds is not None:
                  ds.Close()
            if not use_gdb_items:
              self.assertTrue(
                  feat is None or bool(records),
                  f'{rel_file} at offset {offset}: expected missing feature '
                  'or error/warning',
              )
            else:
              # Exercises sanitizer / no-crash resilience on corrupted index
              # files; PhysicalName = 'NO_FIELD' matches no rows normally, so
              # corruptions either emit records or fall back silently.
              self.assertTrue(ds is not None or bool(records))
              self.assertEqual(bool(records), offset not in silent_offsets)
              if feat is not None:
                self.assertEqual(feat['PhysicalName'], 'NO_FIELD')
          finally:
            target_file.write_bytes(orig_data)

  def test_in_memory_spatial_filter(self) -> None:
    # Upstream: test_ogr_openfilegdb_in_memory_spatial_filter
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with gdal.config_option('OPENFILEGDB_USE_SPATIAL_INDEX', 'NO'):
      # Test building spatial index with GetFeatureCount().
      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('several_polygons')
        self.assertEqual(
            self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
            _SPI_IN_BUILDING,
        )
        lyr.ResetReading()
        self.assertEqual(
            self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
            _SPI_IN_BUILDING,
        )
        self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
        with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
          self.assertEqual(lyr.GetFeatureCount(), 1)
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_COMPLETED,
          )
          self.assertEqual(lyr.GetFeatureCount(), 1)
          self.assertLen(list(lyr), 1)

      # Test iterating without spatial index already built.
      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('several_polygons')
        with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
          feat = lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_IN_BUILDING,
          )
          count = 1
          while lyr.GetNextFeature() is not None:
            count += 1
          self.assertEqual(count, 1)
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_COMPLETED,
          )

      # HAVE_GEOS is enabled in //third_party/gdal/BUILD, so spatial filter is
      # exact and returns 0 features.
      self.assertGreater(ogr.GetGEOSVersionMajor(), 0)
      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('multipolygon')
        with _spatial_filter_rect(lyr, 1.4, 0.4, 1.6, 0.6):
          self.assertEqual(lyr.GetFeatureCount(), 0)

      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('multipolygon')
        with _spatial_filter_rect(lyr, 1.4, 0.4, 1.6, 0.6):
          self.assertEmpty(list(lyr))
          self.assertEqual(lyr.GetFeatureCount(), 0)

      # GetFeature() should not impact spatial index building; SetNextByIndex()
      # works with filtered features once built.
      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('several_polygons')
        with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
          self.assertIsNotNone(lyr.GetFeature(1))
          self.assertIsNotNone(lyr.GetFeature(1))
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_IN_BUILDING,
          )
          while lyr.GetNextFeature() is not None:
            pass
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_COMPLETED,
          )
          self.assertLen(list(lyr), 1)
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_COMPLETED,
          )

        with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
          self.assertEqual(lyr.TestCapability(ogr.OLCFastSetNextByIndex), 1)
          with ogr.ExceptionMgr(useExceptions=False):
            self.assertEqual(
                lyr.SetNextByIndex(-1), ogr.OGRERR_NON_EXISTING_FEATURE
            )
            self.assertEqual(
                lyr.SetNextByIndex(1), ogr.OGRERR_NON_EXISTING_FEATURE
            )
          self.assertEqual(lyr.SetNextByIndex(0), 0)
          feat = lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          self.assertEqual(feat.GetFID(), 1)
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_COMPLETED,
          )

      # SetNextByIndex(), ResetReading(), and indexed SetAttributeFilter()
      # invalidate in-memory spatial index building.
      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('multipolygon')
        self.assertEqual(lyr.SetNextByIndex(3), 0)
        self.assertEqual(
            self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
            _SPI_INVALID,
        )

      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('multipolygon')
        self.assertIsNotNone(lyr.GetNextFeature())
        lyr.ResetReading()
        self.assertEqual(
            self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
            _SPI_INVALID,
        )

      with self._open_gdb(filepath) as ds:
        lyr = ds.GetLayerByName('point')
        with _attribute_filter(lyr, 'id = 1'):
          self.assertEqual(
              self._query_int(ds, f'GetLayerSpatialIndexState {lyr.GetName()}'),
              _SPI_INVALID,
          )

  def test_spx_spatial_filter(self) -> None:
    # Upstream: test_ogr_openfilegdb_spx_spatial_filter
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('several_polygons')
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastSpatialFilter), 1)
      with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
        self.assertEqual(lyr.GetFeatureCount(), 1)
        self.assertLen(list(lyr), 1)

      with _spatial_filter_rect(lyr, 0, 2, 1, 5):
        self.assertEqual(lyr.GetFeatureCount(), 2)

      self.assertEqual(lyr.GetFeatureCount(), 9)
      with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
        self.assertEqual(lyr.GetFeatureCount(), 1)

    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('several_polygons')
      with _spatial_filter_rect(lyr, 0.25, 0.25, 0.5, 0.5):
        self.assertLen(list(lyr), 1)

    # HAVE_GEOS is enabled in //third_party/gdal/BUILD, so spatial filter is
    # exact and returns 0 features.
    self.assertGreater(ogr.GetGEOSVersionMajor(), 0)
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('multipolygon')
      with _spatial_filter_rect(lyr, 1.4, 0.4, 1.6, 0.6):
        self.assertEqual(lyr.GetFeatureCount(), 0)

    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('multipolygon')
      with _spatial_filter_rect(lyr, 1.4, 0.4, 1.6, 0.6):
        self.assertEmpty(list(lyr))
        self.assertEqual(lyr.GetFeatureCount(), 0)

    spx_path = self._test_file_path('fgdb/test_spatial_index.gdb.zip')
    with self._open_gdb(spx_path) as ds:
      lyr = ds.GetLayerByName('test')
      lyr.SetAttributeFilter('id = 1')
      try:
        with _spatial_filter_rect(lyr, 400000, 0, 500100, 4500100):
          self.assertEqual(lyr.GetFeatureCount(), 1)
          self.assertLen(list(lyr), 1)

        lyr.SetSpatialFilterRect(500100, 4500000, 500200, 4500100)
        self.assertEqual(lyr.GetFeatureCount(), 0)
        self.assertEmpty(list(lyr))

        lyr.SetAttributeFilter(None)
        self.assertEqual(lyr.GetFeatureCount(), 154)
      finally:
        lyr.SetAttributeFilter(None)
        lyr.SetSpatialFilter(None)

  def test_read_broken_spx_wrong_index_depth(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_broken_spx_wrong_index_depth
    vsi_dir = self.enter_context(
        gcore_util.VsimemCleanupDir(
            f'/vsimem/openfilegdb_test/{self._testMethodName}'
        )
    )
    dirname = f'{vsi_dir}/broken_spx.gdb'
    with self.driver.Create(dirname, 0, 0, 0, gdal.GDT_Unknown) as ds:
      lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
      for j in range(50):
        for i in range(50):
          feat = ogr.Feature(lyr.GetLayerDefn())
          feat.SetGeometry(ogr.CreateGeometryFromWkt(f'POINT({i} {j})'))
          lyr.CreateFeature(feat)

    # V1 .spx files end with a 22-byte trailer where uint32 at offset +6 is
    # nIndexDepth; patch it from 2 to 1 to trigger fallback handling.
    spx_file = f'{dirname}/a00000009.spx'
    with gdal.VSIFile(spx_file, 'rb+') as fh:
      fh.seek(0, os.SEEK_END)
      pos = fh.tell() - 22 + 6
      fh.seek(pos, os.SEEK_SET)
      self.assertEqual(fh.read(1), b'\x02')
      fh.seek(pos, os.SEEK_SET)
      fh.write(b'\x01')

    with self._open_gdb(dirname) as ds:
      lyr = ds.GetLayer(0)
      with _capture_errors() as records:
        with _spatial_filter_rect(lyr, 0.5, 0.5, 48.5, 48.5):
          count = lyr.GetFeatureCount()
      self.assertEqual(count, 48 * 48)
      self.assertTrue(
          any(
              err_class == gdal.CE_Warning
              and f'Cannot use {spx_file}' in msg
              and 'index depth(=1) is suspicious' in msg
              for err_class, _, msg in records
          ),
          f'Expected suspicious index depth warning for {spx_file}, '
          f'got: {records!r}',
      )

  def test_srid_and_latest_srid(self) -> None:
    # Upstream: test_ogr_openfilegdb_12
    filepath = self._test_file_path('fgdb/test3005.gdb.zip')
    with self._open_gdb(f'/vsizip/{filepath}') as ds:
      lyr = ds.GetLayer(0)
      expected_sr = osr.SpatialReference()
      expected_sr.ImportFromEPSG(3005)
      self.assertEqual(
          lyr.GetSpatialRef().ExportToWkt(), expected_sr.ExportToWkt()
      )

  def test_v9_non_spatial_table(self) -> None:
    # Upstream: test_ogr_openfilegdb_13
    filepath = self._test_file_path('fgdb/ESSENCE_NAIPF_ORI_PROV_sub93.gdb.zip')
    with self._open_gdb(f'/vsizip/{filepath}') as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetName(), 'DDE_ESSEN_NAIPF_ORI_VUE')
      self.assertIsNone(lyr.GetSpatialRef())
      self.assertEqual(lyr.GetGeomType(), ogr.wkbNone)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('GEOCODE'), '-673985,22+745574,77')

  def test_not_nullable_fields(self) -> None:
    # Upstream: test_ogr_openfilegdb_14
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('testnotnullable')
      defn = lyr.GetLayerDefn()
      non_null_defn = defn.GetFieldDefn(
          defn.GetFieldIndex('field_not_nullable')
      )
      self.assertEqual(non_null_defn.IsNullable(), 0)
      self.assertEqual(
          defn.GetFieldDefn(defn.GetFieldIndex('field_nullable')).IsNullable(),
          1,
      )
      self.assertEqual(defn.GetGeomFieldDefn(0).IsNullable(), 0)

  def test_default_values(self) -> None:
    # Upstream: test_ogr_openfilegdb_15
    filepath = self._test_file_path('fgdb/test_default_val.gdb.zip')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      str_defn = defn.GetFieldDefn(defn.GetFieldIndex('STR'))
      self.assertEqual(str_defn.GetDefault(), "'default_val'")
      self.assertEqual(str_defn.GetWidth(), 50)
      self.assertEqual(
          defn.GetFieldDefn(defn.GetFieldIndex('INT32')).GetDefault(),
          '123456788',
      )
      self.assertEqual(
          defn.GetFieldDefn(defn.GetFieldIndex('INT16')).GetDefault(), '12345'
      )
      self.assertStartsWith(
          defn.GetFieldDefn(defn.GetFieldIndex('FLOAT32')).GetDefault(), '1.23'
      )
      self.assertStartsWith(
          defn.GetFieldDefn(defn.GetFieldIndex('FLOAT64')).GetDefault(),
          '1.23456',
      )
      self.assertEqual(
          defn.GetFieldDefn(defn.GetFieldIndex('DATETIME')).GetDefault(),
          "'2015/06/30 12:34:56'",
      )

  def test_sparse_pages(self) -> None:
    # Upstream: test_ogr_openfilegdb_16
    filepath = self._test_file_path('fgdb/sparse.gdb.zip')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      expected_fids = (
          2,
          3,
          4,
          7,
          8,
          9,
          10,
          2049,
          8191,
          16384,
          10000000,
          10000001,
      )
      actual_fids = tuple(feat.GetFID() for feat in lyr)
      self.assertEqual(actual_fids, expected_fids)

      self.assertIsNone(lyr.GetFeature(100000))
      self.assertIsNone(lyr.GetFeature(10000000 - 1))
      self.assertIsNone(lyr.GetNextFeature())

      feat = lyr.GetFeature(16384)
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetFID(), 16384)

  def test_multilinestringzm_dummy_m_array(self) -> None:
    # Upstream: test_ogr_openfilegdb_17
    filepath = self._test_file_path(
        'fgdb/multilinestringzm_with_dummy_m_array.gdb.zip'
    )
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      self.assertIsNotNone(lyr)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      geom = feat.GetGeometryRef()
      self.assertIsNotNone(geom)
      self.assertEqual(geom.GetGeometryType(), ogr.wkbMultiLineString25D)
      self.assertTrue(geom.Is3D())
      self.assertFalse(geom.IsMeasured())
      self.assertEqual(geom.GetGeometryCount(), 1)

  def _assert_layer_geometries_match_csv(
      self,
      gdb_rel_path: str,
      layer_name: str | None,
      csv_rel_path: str,
  ) -> None:
    gdb_path = self._test_file_path(gdb_rel_path)
    csv_path = self._test_file_path(csv_rel_path)
    with (
        self._open_gdb(gdb_path) as ds,
        gdal.OpenEx(
            csv_path, gdal.OF_VECTOR | gdal.OF_READONLY, allowed_drivers=['CSV']
        ) as ds_ref,
    ):
      lyr = ds.GetLayerByName(layer_name) if layer_name else ds.GetLayer(0)
      lyr_ref = ds_ref.GetLayer(0)
      self.assertIsNotNone(lyr)
      self.assertIsNotNone(lyr_ref)
      feat_count = lyr.GetFeatureCount()
      self.assertGreater(feat_count, 0)
      self.assertEqual(feat_count, lyr_ref.GetFeatureCount())
      for idx, feat in enumerate(lyr):
        feat_ref = lyr_ref.GetNextFeature()
        self.assertIsNotNone(feat_ref, f'Missing reference feature at {idx}')
        with self.subTest(layer=layer_name or '0', feature_index=idx):
          self._assert_geometry_equal(
              feat.GetGeometryRef(),
              feat_ref.GetGeometryRef(),
          )
      self.assertIsNone(lyr_ref.GetNextFeature())

  @parameterized.named_parameters(
      dict(
          testcase_name='curves_line',
          gdb_rel_path='fgdb/curves.gdb',
          layer_name='line',
          csv_rel_path='fgdb/curves_line.csv',
      ),
      dict(
          testcase_name='curves_polygon',
          gdb_rel_path='fgdb/curves.gdb',
          layer_name='polygon',
          csv_rel_path='fgdb/curves_polygon.csv',
      ),
      dict(
          testcase_name='curve_circle_by_center',
          gdb_rel_path='fgdb/curve_circle_by_center.gdb',
          layer_name=None,
          csv_rel_path='fgdb/curve_circle_by_center.csv',
      ),
  )
  def test_read_curves(
      self, gdb_rel_path: str, layer_name: str | None, csv_rel_path: str
  ) -> None:
    # Upstream: test_ogr_openfilegdb_18
    self._assert_layer_geometries_match_csv(
        gdb_rel_path, layer_name, csv_rel_path
    )

  def test_open_dot_current_directory(self) -> None:
    # Upstream: test_ogr_openfilegdb_19
    curves_dir = self._test_file_path('fgdb/curves.gdb')
    old_cwd = os.getcwd()
    os.chdir(curves_dir)
    try:
      with self._open_gdb('.') as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetLayerCount(), 2)
    finally:
      os.chdir(old_cwd)

  @parameterized.named_parameters(
      dict(
          testcase_name='m_not_closing',
          gdb_rel_path='fgdb/filegdb_polygonzm_m_not_closing_with_curves.gdb',
          csv_rel_path=(
              'fgdb/filegdb_polygonzm_m_not_closing_with_curves.gdb.csv'
          ),
      ),
      dict(
          testcase_name='nan_m',
          gdb_rel_path='fgdb/filegdb_polygonzm_nan_m_with_curves.gdb',
          csv_rel_path='fgdb/filegdb_polygonzm_nan_m_with_curves.gdb.csv',
      ),
  )
  def test_polygonzm_with_curves(
      self, gdb_rel_path: str, csv_rel_path: str
  ) -> None:
    # Upstream: test_ogr_openfilegdb_20
    self._assert_layer_geometries_match_csv(gdb_rel_path, None, csv_rel_path)

  def test_select_fid_column_ogrsql(self) -> None:
    # Upstream: test_ogr_openfilegdb_21
    filepath = self._test_file_path('fgdb/curves.gdb')
    with self._open_gdb(filepath) as ds:
      with ds.ExecuteSQL(
          'SELECT OBJECTID FROM polygon WHERE OBJECTID = 2'
      ) as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFID(), 2)
        self.assertIsNone(sql_lyr.GetNextFeature())

      lyr = ds.GetLayerByName('polygon')
      with _attribute_filter(lyr, 'OBJECTID = 2'):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFID(), 2)

  def test_weird_winding_order(self) -> None:
    # Upstream: test_ogr_openfilegdb_weird_winding_order
    # HAVE_GEOS is enabled in //third_party/gdal/BUILD.
    self.assertGreater(ogr.GetGEOSVersionMajor(), 0)
    filepath = self._test_file_path('fgdb/weird_winding_order_fgdb.zip')
    with self._open_gdb(f'/vsizip/{filepath}/roads_clip Drawing.gdb') as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      geom = feat.GetGeometryRef()
      self.assertIsNotNone(geom)
      self.assertEqual(geom.GetGeometryCount(), 1)
      self.assertEqual(geom.GetGeometryRef(0).GetGeometryCount(), 17)
      self.assertTrue(geom.IsValid())

  def test_utc_datetime(self) -> None:
    # Upstream: test_ogr_openfilegdb_utc_datetime
    filepath = self._test_file_path('fgdb/testdatetimeutc.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(
          feat.GetFieldAsString('EditDate'), '2020/06/22 07:49:36+00'
      )

  def test_field_alias(self) -> None:
    # Upstream: test_ogr_fgdb_alias
    filepath = self._test_file_path('fgdb/field_alias.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      for field_name, expected_alias in _FIELD_ALIASES:
        with self.subTest(field=field_name):
          idx = defn.GetFieldIndex(field_name)
          self.assertGreaterEqual(idx, 0)
          self.assertEqual(
              defn.GetFieldDefn(idx).GetAlternativeName(), expected_alias
          )

  def _assert_domains_match(self, ds: gdal.Dataset) -> None:
    self.assertCountEqual(
        ds.GetFieldDomainNames(),
        ('MedianType', 'RoadSurfaceType', 'SpeedLimit'),
    )
    with gdal.quiet_errors():
      self.assertIsNone(ds.GetFieldDomain('i_dont_exist'))

    lyr = ds.GetLayer(0)
    lyr_defn = lyr.GetLayerDefn()

    speed_defn = lyr_defn.GetFieldDefn(lyr_defn.GetFieldIndex('MaxSpeed'))
    self.assertEqual(speed_defn.GetDomainName(), 'SpeedLimit')
    speed_domain = ds.GetFieldDomain('SpeedLimit')
    self.assertIsNotNone(speed_domain)
    self.assertEqual(speed_domain.GetName(), 'SpeedLimit')
    self.assertEqual(
        speed_domain.GetDescription(), 'The maximun speed of the road'
    )
    self.assertEqual(speed_domain.GetDomainType(), ogr.OFDT_RANGE)
    self.assertEqual(speed_domain.GetFieldType(), speed_defn.GetType())
    self.assertEqual(speed_domain.GetFieldSubType(), speed_defn.GetSubType())
    self.assertEqual(speed_domain.GetMinAsDouble(), 40.0)
    self.assertEqual(speed_domain.GetMaxAsDouble(), 100.0)

    median_defn = lyr_defn.GetFieldDefn(lyr_defn.GetFieldIndex('MedianType'))
    self.assertEqual(median_defn.GetDomainName(), 'MedianType')
    median_domain = ds.GetFieldDomain('MedianType')
    self.assertIsNotNone(median_domain)
    self.assertEqual(median_domain.GetName(), 'MedianType')
    self.assertEqual(median_domain.GetDescription(), 'Road median types.')
    self.assertEqual(median_domain.GetDomainType(), ogr.OFDT_CODED)
    self.assertEqual(median_domain.GetFieldType(), median_defn.GetType())
    self.assertEqual(median_domain.GetFieldSubType(), median_defn.GetSubType())
    self.assertEqual(
        median_domain.GetEnumeration(), {'0': 'None', '1': 'Cement'}
    )

  def test_read_domains(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_domains
    filepath = self._test_file_path('fgdb/Domains.gdb')
    with self._open_gdb(filepath) as ds:
      self._assert_domains_match(ds)

  def test_write_domains_from_other_gdb(self) -> None:
    # Upstream: test_ogr_openfilegdb_write_domains_from_other_gdb
    src_path = self._test_file_path('fgdb/Domains.gdb')
    out_dir = (
        pathlib.Path(self.create_tempdir().full_path)
        / 'test_ogr_fgdb_write_domains.gdb'
    )
    with gdal.VectorTranslate(
        str(out_dir), src_path, options='-f OpenFileGDB'
    ) as ds:
      self._assert_domains_match(ds)
      self.assertEqual(ds.TestCapability(ogr.ODsCAddFieldDomain), 1)
      self.assertEqual(ds.TestCapability(ogr.ODsCDeleteFieldDomain), 1)
      self.assertEqual(ds.TestCapability(ogr.ODsCUpdateFieldDomain), 1)

      with gdal.quiet_errors():
        self.assertFalse(ds.DeleteFieldDomain('not_existing'))

      coded_domain = ogr.CreateCodedFieldDomain(
          'unused_domain',
          'desc',
          ogr.OFTInteger,
          ogr.OFSTNone,
          {'1': 'one', '2': None},
      )
      self.assertTrue(ds.AddFieldDomain(coded_domain))
      self.assertTrue(ds.DeleteFieldDomain('unused_domain'))
      with gdal.quiet_errors():
        self.assertIsNone(ds.GetFieldDomain('unused_domain'))

      range_domain = ogr.CreateRangeFieldDomain(
          'SpeedLimit', 'desc', ogr.OFTInteger, ogr.OFSTNone, 1, True, 2, True
      )
      self.assertTrue(ds.UpdateFieldDomain(range_domain))

    with self._open_gdb(out_dir) as ds:
      with gdal.quiet_errors():
        self.assertIsNone(ds.GetFieldDomain('unused_domain'))
      self.assertCountEqual(
          ds.GetFieldDomainNames(),
          ('MedianType', 'RoadSurfaceType', 'SpeedLimit'),
      )
      median_domain = ds.GetFieldDomain('MedianType')
      self.assertIsNotNone(median_domain)
      self.assertEqual(
          median_domain.GetEnumeration(), {'0': 'None', '1': 'Cement'}
      )
      self.assertIsNotNone(ds.GetFieldDomain('RoadSurfaceType'))
      domain = ds.GetFieldDomain('SpeedLimit')
      self.assertIsNotNone(domain)
      self.assertEqual(domain.GetDescription(), 'desc')
      self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
      self.assertEqual(domain.GetMinAsDouble(), 1.0)
      self.assertEqual(domain.GetMaxAsDouble(), 2.0)

  def test_read_layer_hierarchy(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_layer_hierarchy
    filepath = self._test_file_path('fgdb/featuredataset.gdb')
    with self._open_gdb(filepath) as ds:
      rg = ds.GetRootGroup()
      self.assertIsNotNone(rg)
      self.assertEqual(rg.GetGroupNames(), ['fd1', 'fd2'])
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'Group not_existing does not exist'
          ),
      ):
        rg.OpenGroup('not_existing')

      fd1 = rg.OpenGroup('fd1')
      self.assertIsNotNone(fd1)
      self.assertEqual(fd1.GetVectorLayerNames(), ['fd1_lyr1', 'fd1_lyr2'])
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'Vector layer not_existing does not exist'
          ),
      ):
        fd1.OpenVectorLayer('not_existing')
      self.assertEmpty(fd1.GetGroupNames())

      fd1_lyr1 = fd1.OpenVectorLayer('fd1_lyr1')
      self.assertIsNotNone(fd1_lyr1)
      self.assertEqual(fd1_lyr1.GetName(), 'fd1_lyr1')

      fd1_lyr2 = fd1.OpenVectorLayer('fd1_lyr2')
      self.assertIsNotNone(fd1_lyr2)
      self.assertEqual(fd1_lyr2.GetName(), 'fd1_lyr2')

      fd2 = rg.OpenGroup('fd2')
      self.assertIsNotNone(fd2)
      self.assertEqual(fd2.GetVectorLayerNames(), ['fd2_lyr'])
      fd2_lyr = fd2.OpenVectorLayer('fd2_lyr')
      self.assertIsNotNone(fd2_lyr)
      self.assertEqual(fd2_lyr.GetName(), 'fd2_lyr')

      self.assertEqual(rg.GetVectorLayerNames(), ['standalone'])
      standalone = rg.OpenVectorLayer('standalone')
      self.assertIsNotNone(standalone)
      self.assertEqual(standalone.GetName(), 'standalone')

  @parameterized.named_parameters(
      dict(
          testcase_name='v10',
          rel_path='testopenfilegdb.gdb.zip',
          default_count=37,
          all_count=44,
          system_tables=_V10_SYSTEM_TABLES,
          public_tables=('linestring', 'point', 'multipoint'),
      ),
      dict(
          testcase_name='v9',
          rel_path='testopenfilegdb93.gdb.zip',
          default_count=22,
          all_count=31,
          system_tables=_V9_SYSTEM_TABLES,
          public_tables=('linestring', 'point', 'multipoint'),
      ),
      dict(
          testcase_name='non_spatial_outside_gdb_items',
          rel_path='fgdb/table_outside_gdbitems.gdb',
          default_count=3,
          all_count=10,
          system_tables=_V10_SYSTEM_TABLES,
          public_tables=('aquaduct', 'flat_table1', 'flat_table2'),
      ),
  )
  def test_list_all_tables(
      self,
      rel_path: str,
      default_count: int,
      all_count: int,
      system_tables: tuple[str, ...],
      public_tables: tuple[str, ...],
  ) -> None:
    # Upstream: test_ogr_openfilegdb_list_all_tables_v10,
    # test_ogr_openfilegdb_list_all_tables_v9,
    # test_ogr_openfilegdb_non_spatial_table_outside_gdb_items
    filepath = self._test_file_path(rel_path)
    with self._open_gdb(filepath) as ds:
      self.assertEqual(ds.GetLayerCount(), default_count)
      default_names = {
          ds.GetLayer(i).GetName() for i in range(ds.GetLayerCount())
      }
      self.assertTrue(default_names.isdisjoint(system_tables))
      self.assertTrue(set(public_tables).issubset(default_names))

    with self._open_gdb(
        filepath, open_options=['LIST_ALL_TABLES=YES']
    ) as ds_all:
      self.assertEqual(ds_all.GetLayerCount(), all_count)
      all_names = {
          ds_all.GetLayer(i).GetName() for i in range(ds_all.GetLayerCount())
      }
      private_names = {
          ds_all.GetLayer(i).GetName()
          for i in range(ds_all.GetLayerCount())
          if ds_all.IsLayerPrivate(i)
      }
      self.assertTrue(set(system_tables).issubset(all_names))
      self.assertTrue(set(system_tables).issubset(private_names))
      self.assertTrue(set(public_tables).issubset(all_names))
      self.assertTrue(private_names.isdisjoint(public_tables))

  def test_strings_utf16(self) -> None:
    # Upstream: test_ogr_openfilegdb_strings_utf16
    filepath = self._test_file_path('fgdb/test_utf16.gdb.zip')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      fld_defn = lyr.GetLayerDefn().GetFieldDefn(0)
      self.assertEqual(fld_defn.GetDefault(), "'éven'")
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['str'], 'évenéven')

  def test_inconsistent_crs_feature_dataset_and_table(self) -> None:
    # Upstream:
    # test_ogr_openfilegdb_inconsistent_crs_feature_dataset_and_feature_table
    filepath = self._test_file_path(
        'fgdb/inconsistent_crs_feature_dataset_and_feature_table.gdb'
    )
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      srs = lyr.GetSpatialRef()
      self.assertIsNotNone(srs)
      self.assertEqual(srs.GetAuthorityCode(None), '4326')

  def test_spx_zero_in_value_count_trailer(self) -> None:
    # Upstream: test_ogr_openfilegdb_spx_zero_in_value_count_trailer
    filepath = self._test_file_path('fgdb/spx_zero_in_value_count_trailer.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      with _spatial_filter_rect(lyr, 1, 1, 2, 2):
        self.assertEqual(lyr.GetFeatureCount(), 1)

  def test_shape_length_shape_area_default(self) -> None:
    # Upstream:
    # test_ogr_openfilegdb_shape_length_shape_area_as_default_in_field_defn
    filepath = self._test_file_path(
        'fgdb/filegdb_polygonzm_m_not_closing_with_curves.gdb'
    )
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      self.assertEqual(
          defn.GetFieldDefn(defn.GetFieldIndex('Shape_Area')).GetDefault(),
          'FILEGEODATABASE_SHAPE_AREA',
      )
      self.assertEqual(
          defn.GetFieldDefn(defn.GetFieldIndex('Shape_Length')).GetDefault(),
          'FILEGEODATABASE_SHAPE_LENGTH',
      )

  class _RelationshipSpec(NamedTuple):
    name: str
    left_table: str
    right_table: str
    mapping_table: str
    cardinality: int
    rel_type: int
    left_fields: tuple[str, ...]
    right_fields: tuple[str, ...]
    related_type: str
    left_mapping_fields: tuple[str, ...] | None = None
    right_mapping_fields: tuple[str, ...] | None = None
    check_none_mapping_fields: bool = False
    forward_label: str | None = None
    backward_label: str | None = None

  def _assert_relationship(
      self, ds: gdal.Dataset, spec: _RelationshipSpec
  ) -> None:
    rel = ds.GetRelationship(spec.name)
    self.assertIsNotNone(rel)
    self.assertEqual(rel.GetName(), spec.name)
    self.assertEqual(rel.GetLeftTableName(), spec.left_table)
    self.assertEqual(rel.GetRightTableName(), spec.right_table)
    self.assertEqual(rel.GetMappingTableName(), spec.mapping_table)
    self.assertEqual(rel.GetCardinality(), spec.cardinality)
    self.assertEqual(rel.GetType(), spec.rel_type)
    self.assertEqual(tuple(rel.GetLeftTableFields()), spec.left_fields)
    self.assertEqual(tuple(rel.GetRightTableFields()), spec.right_fields)
    if spec.check_none_mapping_fields:
      self.assertIsNone(rel.GetLeftMappingTableFields())
      self.assertIsNone(rel.GetRightMappingTableFields())
    if spec.left_mapping_fields is not None:
      self.assertEqual(
          tuple(rel.GetLeftMappingTableFields()), spec.left_mapping_fields
      )
    if spec.right_mapping_fields is not None:
      self.assertEqual(
          tuple(rel.GetRightMappingTableFields()), spec.right_mapping_fields
      )
    if spec.forward_label is not None:
      self.assertEqual(rel.GetForwardPathLabel(), spec.forward_label)
    if spec.backward_label is not None:
      self.assertEqual(rel.GetBackwardPathLabel(), spec.backward_label)
    self.assertEqual(rel.GetRelatedTableType(), spec.related_type)

  def test_read_relationships(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_relationships
    domains_path = self._test_file_path('fgdb/Domains.gdb')
    with self._open_gdb(domains_path) as ds:
      self.assertIsNone(ds.GetRelationshipNames())

    rel_path = self._test_file_path('fgdb/relationships.gdb')
    with self._open_gdb(rel_path) as ds:
      self.assertCountEqual(
          ds.GetRelationshipNames(),
          (
              'composite_many_to_many',
              'composite_one_to_many',
              'composite_one_to_one',
              'simple_attributed',
              'simple_backward_message_direction',
              'simple_both_message_direction',
              'simple_forward_message_direction',
              'simple_many_to_many',
              'simple_one_to_many',
              'simple_relationship_one_to_one',
              'points__ATTACHREL',
          ),
      )
      self.assertIsNone(ds.GetRelationship('xxxx'))

      relationships = (
          self._RelationshipSpec(
              name='simple_relationship_one_to_one',
              left_table='table1',
              right_table='table2',
              mapping_table='',
              cardinality=gdal.GRC_ONE_TO_ONE,
              rel_type=gdal.GRT_ASSOCIATION,
              left_fields=('pk',),
              right_fields=('parent_pk',),
              check_none_mapping_fields=True,
              forward_label='my forward path label',
              backward_label='my backward path label',
              related_type='features',
          ),
          self._RelationshipSpec(
              name='simple_one_to_many',
              left_table='table1',
              right_table='table2',
              mapping_table='',
              cardinality=gdal.GRC_ONE_TO_MANY,
              rel_type=gdal.GRT_ASSOCIATION,
              left_fields=('pk',),
              right_fields=('parent_pk',),
              related_type='features',
          ),
          self._RelationshipSpec(
              name='simple_many_to_many',
              left_table='table1',
              right_table='table2',
              mapping_table='simple_many_to_many',
              cardinality=gdal.GRC_MANY_TO_MANY,
              rel_type=gdal.GRT_ASSOCIATION,
              left_fields=('pk',),
              right_fields=('parent_pk',),
              left_mapping_fields=('origin_foreign_key',),
              right_mapping_fields=('destination_foreign_key',),
              related_type='features',
          ),
          self._RelationshipSpec(
              name='composite_one_to_one',
              left_table='table1',
              right_table='table3',
              mapping_table='',
              cardinality=gdal.GRC_ONE_TO_ONE,
              rel_type=gdal.GRT_COMPOSITE,
              left_fields=('pk',),
              right_fields=('parent_pk',),
              related_type='features',
          ),
          self._RelationshipSpec(
              name='composite_one_to_many',
              left_table='table5',
              right_table='table4',
              mapping_table='',
              cardinality=gdal.GRC_ONE_TO_MANY,
              rel_type=gdal.GRT_COMPOSITE,
              left_fields=('pk',),
              right_fields=('parent_pk',),
              related_type='features',
          ),
          self._RelationshipSpec(
              name='composite_many_to_many',
              left_table='table6',
              right_table='table7',
              mapping_table='composite_many_to_many',
              cardinality=gdal.GRC_MANY_TO_MANY,
              rel_type=gdal.GRT_COMPOSITE,
              left_fields=('pk',),
              right_fields=('parent_pk',),
              left_mapping_fields=('origin_foreign_key',),
              right_mapping_fields=('dest_foreign_key',),
              related_type='features',
          ),
          self._RelationshipSpec(
              name='points__ATTACHREL',
              left_table='points',
              right_table='points__ATTACH',
              mapping_table='',
              cardinality=gdal.GRC_ONE_TO_MANY,
              rel_type=gdal.GRT_COMPOSITE,
              left_fields=('OBJECTID',),
              right_fields=('REL_OBJECTID',),
              forward_label='attachment',
              backward_label='object',
              related_type='media',
          ),
      )
      for spec in relationships:
        with self.subTest(relationship=spec.name):
          self._assert_relationship(ds, spec)

  def test_read_readonly_in_update_mode(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_readonly_in_update_mode
    if sys.platform != 'linux':
      self.skipTest('Permission check relies on Linux chmod semantics')
    if os.getuid() == 0:
      self.skipTest(
          'Cannot verify read-only chmod restriction when running as root'
      )

    src_dir = self._test_file_path('fgdb/Domains.gdb')
    ro_dir = pathlib.Path(self.create_tempdir().full_path) / 'testreadonly.gdb'
    shutil.copytree(src_dir, ro_dir)

    try:
      os.chmod(ro_dir, 0o555)
      for child in ro_dir.iterdir():
        os.chmod(child, 0o444)

      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'Cannot open .*a00000001\.gdbtable: Permission denied',
          ),
      ):
        with self._open_gdb(ro_dir, update=True) as _:
          pass

      with self._open_gdb(ro_dir, update=False) as ds:
        self.assertIsNotNone(ds)
        self.assertEqual(ds.GetLayerCount(), 1)

      # Make only system catalog/items tables writable, keeping Roads read-only.
      os.chmod(ro_dir, 0o755)
      for child in ro_dir.iterdir():
        if child.name.startswith(('a00000001.', 'a00000004.')):
          os.chmod(child, 0o644)

      with self._open_gdb(ro_dir, update=True) as ds:
        lyr = ds.GetLayer(0)
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(
                RuntimeError,
                r'Cannot open Roads in update mode, but only in read-only',
            ),
        ):
          lyr.TestCapability(ogr.OLCSequentialWrite)
        self.assertEqual(lyr.TestCapability(ogr.OLCSequentialWrite), 0)
    finally:
      os.chmod(ro_dir, 0o755)
      for child in ro_dir.iterdir():
        os.chmod(child, 0o644)

  def test_read_int64(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_int64
    filepath = self._test_file_path('fgdb/arcgis_pro_32_types.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('big_int')
      defn = lyr.GetLayerDefn()
      fld_defn = defn.GetFieldDefn(defn.GetFieldIndex('big'))
      self.assertEqual(fld_defn.GetType(), ogr.OFTInteger64)
      self.assertEqual(fld_defn.GetDefault(), '1234567890123456')

      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['short'], 32767)
      self.assertEqual(feat['long'], 2147483647)
      self.assertEqual(feat['big'], 9007199254740991)
      # Deltas 1e32 and 1e302 match ~1e-6 relative precision for float32/64 max.
      self.assertAlmostEqual(feat['float'], 3.4e38, delta=1e32)
      self.assertAlmostEqual(feat['double'], 1.7976931348623e308, delta=1e302)

      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['short'], -32768)
      self.assertEqual(feat['long'], -2147483647)
      self.assertEqual(feat['big'], -9007199254740991)
      self.assertAlmostEqual(feat['float'], -3.4e38, delta=1e32)
      self.assertAlmostEqual(feat['double'], -1.7976931348623e308, delta=1e302)

      with _attribute_filter(lyr, 'big > 0'):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat['big'], 9007199254740991)

      with _attribute_filter(lyr, 'big < 0'):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat['big'], -9007199254740991)

      with ds.ExecuteSQL('SELECT MIN(big), MAX(big) FROM big_int') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat['MIN_big'], -9007199254740991)
        self.assertEqual(feat['MAX_big'], 9007199254740991)

  def _assert_datetime_layer_schema(
      self,
      layer: ogr.Layer,
      specs: tuple[tuple[str, int, str], ...],
  ) -> None:
    defn = layer.GetLayerDefn()
    layer_name = layer.GetName()
    for col, ftype, default in specs:
      with self.subTest(layer=layer_name, col=col):
        fld_defn = defn.GetFieldDefn(defn.GetFieldIndex(col))
        self.assertEqual(fld_defn.GetType(), ftype)
        self.assertEqual(fld_defn.GetDefault(), default)

  def test_read_new_datetime_types(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_new_datetime_types
    filepath = self._test_file_path('fgdb/arcgis_pro_32_types.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('date_types')
      self._assert_datetime_layer_schema(
          lyr,
          (
              ('date', ogr.OFTDateTime, "'2023/02/01 04:05:06'"),
              ('date_only', ogr.OFTDate, "'2023/02/01'"),
              ('time_only', ogr.OFTTime, "'04:05:06'"),
              (
                  'timestamp_offset',
                  ogr.OFTDateTime,
                  "'2023/02/01 04:05:06.000+06:00'",
              ),
          ),
      )

      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['date'], '2023/11/29 13:14:15+00')
      self.assertEqual(feat['date_only'], '2023/11/29')
      self.assertEqual(feat['time_only'], '13:14:15')
      self.assertEqual(feat['timestamp_offset'], '2023/11/29 13:14:15-05')

      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['date'], '2023/12/31 00:01:01+00')
      self.assertEqual(feat['date_only'], '2023/12/31')
      self.assertEqual(feat['time_only'], '00:01:01')
      self.assertEqual(feat['timestamp_offset'], '2023/12/31 00:01:01+10')

      for col, exp_min, exp_max in (
          ('date', '1901/01/01 00:01:01+00', '2023/12/31 00:01:01+00'),
          ('date_only', '1901/01/01', '2023/12/31'),
          ('time_only', '00:01:01', '13:14:15'),
          (
              'timestamp_offset',
              '1901/01/01 00:01:01+10',
              '2023/12/31 00:01:01+10',
          ),
      ):
        with (
            self.subTest(col=col),
            ds.ExecuteSQL(
                f'SELECT MIN("{col}"), MAX("{col}") FROM date_types'
            ) as sql_lyr,
        ):
          feat = sql_lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          self.assertEqual(feat[f'MIN_{col}'], exp_min)
          self.assertEqual(feat[f'MAX_{col}'], exp_max)

      lyr_hp = ds.GetLayerByName('date_types_high_precision')
      self._assert_datetime_layer_schema(
          lyr_hp,
          (
              ('date', ogr.OFTDateTime, "'2023/01/02 04:05:06'"),
              ('date_only', ogr.OFTDate, "'2023/01/02'"),
              ('time_only', ogr.OFTTime, "'04:05:06'"),
              (
                  'timestamp_offset',
                  ogr.OFTDateTime,
                  "'2023/01/02 04:05:06.000+06:00'",
              ),
          ),
      )

      feat = lyr_hp.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['date'], '2023/11/29 13:14:15.678+00')
      self.assertEqual(feat['date_only'], '2023/11/29')
      self.assertEqual(feat['time_only'], '13:14:15')
      self.assertEqual(feat['timestamp_offset'], '2023/11/29 13:14:15-05')

      feat = lyr_hp.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['date'], '2023/12/31 00:01:01.001+00')
      self.assertEqual(feat['date_only'], '2023/12/31')
      self.assertEqual(feat['time_only'], '00:01:01')
      self.assertEqual(feat['timestamp_offset'], '2023/12/31 00:01:01+10')

      with ds.ExecuteSQL(
          'SELECT MIN("timestamp_offset"), MAX("timestamp_offset") '
          'FROM date_types_high_precision'
      ) as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat['MIN_timestamp_offset'], '1900/12/31 14:01:01')
        self.assertEqual(feat['MAX_timestamp_offset'], '2023/12/30 14:01:01')

  def test_get_extent_and_get_extent_3d(self) -> None:
    # Upstream: test_ogr_openfilegdb_get_extent_getextent3d
    filepath = self._test_file_path('testopenfilegdb.gdb.zip')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayerByName('point')
      self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent))
      self.assertEqual(lyr.GetExtent(), (1.0, 1.0, 2.0, 2.0))
      self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent3D))
      self.assertEqual(
          lyr.GetExtent3D(), (1.0, 1.0, 2.0, 2.0, math.inf, -math.inf)
      )

      lyr = ds.GetLayerByName('point25D')
      self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent))
      self.assertEqual(lyr.GetExtent(), (1.0, 1.0, 2.0, 2.0))
      self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent3D))
      self.assertEqual(lyr.GetExtent3D(), (1.0, 1.0, 2.0, 2.0, 3.0, 3.0))
      with _attribute_filter(lyr, '1=1'):
        self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent3D), 0)
        # Delta 1e-6 bounds IEEE-754 coordinate extent rounding.
        for got, exp in zip(
            lyr.GetExtent3D(), (1.0, 1.0, 2.0, 2.0, 3.0, 3.0), strict=True
        ):
          self.assertAlmostEqual(got, exp, delta=1e-6)

      lyr = ds.GetLayerByName('none')
      self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent), 1)
      self.assertIsNone(lyr.GetExtent(can_return_null=True))
      self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent3D), 0)
      self.assertIsNone(lyr.GetExtent3D(can_return_null=True))

    pro_path = self._test_file_path('fgdb/arcgis_pro_32_types.gdb')
    with self._open_gdb(pro_path) as ds:
      lyr = ds.GetLayerByName('date_types')
      self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent))
      expected_2d = (
          -109.26723474299996,
          -104.69136945899999,
          40.407842636000055,
          41.69941751400006,
      )
      # Delta 1e-6 covers floating-point formatting noise in stored extents.
      for got, exp in zip(lyr.GetExtent(), expected_2d, strict=True):
        self.assertAlmostEqual(got, exp, delta=1e-6)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent3D), 0)
      expected_3d = expected_2d + (0.0, 0.0)
      for got, exp in zip(lyr.GetExtent3D(), expected_3d, strict=True):
        self.assertAlmostEqual(got, exp, delta=1e-6)

  def test_try_identify_vector_as_raster(self) -> None:
    # Upstream: test_ogr_openfilegdb_try_identify_vector_as_raster
    filepath = self._test_file_path('fgdb/empty_polygon.gdb')
    drv = gdal.IdentifyDriverEx(filepath)
    self.assertIsNotNone(drv)
    self.assertEqual(drv.ShortName, 'OpenFileGDB')
    self.assertIsNone(gdal.IdentifyDriverEx(filepath, gdal.OF_RASTER))

  def test_read_cdf(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_cdf
    # Proprietary ESRI FileGDB driver is not built in Google3.
    self.assertIsNone(gdal.GetDriverByName('FileGDB'))
    filepath = self._test_file_path('fgdb/with_cdf.gdb')
    with _capture_errors() as records:
      with self.assertRaisesRegex(RuntimeError, r'not recognized'):
        with gdal.OpenEx(filepath, gdal.OF_VECTOR | gdal.OF_READONLY) as _:
          pass
    self.assertTrue(
        any(
            'file using Compressed Data Format (CDF) that is unhandled by the '
            'OpenFileGDB driver, but could be handled by the FileGDB driver'
            in msg
            for _, _, msg in records
        ),
        f'Expected CDF warning, got: {records!r}',
    )

  def test_read_objectid_64bit_non_sparse(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_objectid_64bit_non_sparse
    # (test_ogr_openfilegdb_read_objectid_64bit_non_sparse_test_ogrsf omitted:
    # test_ogrsf binary is excluded from Google3's //third_party/gdal).
    filepath = self._test_file_path('fgdb/objectid64/3features.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 3)

      feat1 = lyr.GetNextFeature()
      self.assertIsNotNone(feat1)
      self.assertEqual(feat1.GetFID(), 1)
      self.assertAlmostEqual(
          feat1['Shape_Length'], 3140.05912327677, delta=1e-6
      )
      self.assertAlmostEqual(feat1['Shape_Area'], 217981.09775568, delta=1e-6)
      geom1 = feat1.GetGeometryRef()
      self.assertIsNotNone(geom1)
      self.assertEqual(geom1.GetGeometryType(), ogr.wkbMultiPolygon25D)
      self.assertFalse(geom1.IsEmpty())
      minx, maxx, miny, maxy = geom1.GetEnvelope()

      feat2 = lyr.GetNextFeature()
      self.assertIsNotNone(feat2)
      self.assertEqual(feat2.GetFID(), 2)
      self.assertAlmostEqual(feat2['Shape_Length'], 3078.7376875286, delta=1e-6)
      self.assertAlmostEqual(feat2['Shape_Area'], 538056.426171967, delta=1e-6)
      geom2 = feat2.GetGeometryRef()
      self.assertIsNotNone(geom2)
      self.assertEqual(geom2.GetGeometryType(), ogr.wkbMultiPolygon25D)
      self.assertFalse(geom2.IsEmpty())

      feat3 = lyr.GetNextFeature()
      self.assertIsNotNone(feat3)
      self.assertEqual(feat3.GetFID(), 3)
      self.assertAlmostEqual(feat3['Shape_Length'], 3330.7300497069, delta=1e-6)
      self.assertAlmostEqual(feat3['Shape_Area'], 631040.074244291, delta=1e-6)
      geom3 = feat3.GetGeometryRef()
      self.assertIsNotNone(geom3)
      self.assertEqual(geom3.GetGeometryType(), ogr.wkbMultiPolygon25D)
      self.assertFalse(geom3.IsEmpty())

      self.assertIsNone(lyr.GetNextFeature())

      center_x = (minx + maxx) / 2
      center_y = (miny + maxy) / 2
      with _spatial_filter_rect(lyr, center_x, center_y, center_x, center_y):
        lyr.ResetReading()
        self.assertEqual(lyr.GetFeatureCount(), 1)
        # GetFeatureCount() advances the spatial-index iterator; reset first.
        lyr.ResetReading()
        filt_feat = lyr.GetNextFeature()
        self.assertIsNotNone(filt_feat)
        self.assertEqual(filt_feat.GetFID(), 1)
        self.assertIsNone(lyr.GetNextFeature())

    # Blaze runfiles are read-only; copy and chmod 0o644 so V4 format rejection
    # is exercised rather than POSIX EACCES.
    writable_dir = (
        pathlib.Path(self.create_tempdir().full_path) / '3features.gdb'
    )
    shutil.copytree(filepath, writable_dir)
    os.chmod(writable_dir, 0o755)
    for child in writable_dir.iterdir():
      os.chmod(child, 0o644)

    with self._open_gdb(writable_dir, update=True) as ds:
      lyr = ds.GetLayer(0)
      with (
          gdal.ExceptionMgr(useExceptions=False),
          ogr.ExceptionMgr(useExceptions=False),
          _capture_errors() as records,
      ):
        self.assertEqual(lyr.TestCapability(ogr.OLCSequentialWrite), 0)
      msgs = [
          msg for err_class, _, msg in records if err_class == gdal.CE_Failure
      ]
      self.assertTrue(
          any(
              'Version 4 of the FileGeodatabase format is not supported for '
              'update.'
              in msg
              for msg in msgs
          ),
          f'Expected V4 update-mode error, got: {records!r}',
      )
      self.assertTrue(
          any('Cannot open testpolygon in update mode' in msg for msg in msgs),
          f'Expected read-only fallback error, got: {records!r}',
      )
      self.assertEqual(lyr.TestCapability(ogr.OLCSequentialWrite), 0)

  def test_read_objectid_64bit_sparse(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_objectid_64bit_sparse
    # (test_ogr_openfilegdb_read_objectid_64bit_sparse_test_ogrsf omitted:
    # test_ogrsf binary is excluded from Google3's //third_party/gdal).
    filepath = self._test_file_path('fgdb/objectid64/with_holes_8.gdb')
    with self._open_gdb(filepath) as ds:
      for lyr_name, expected_fid in (
          ('with_holes_8_a', 123456),
          ('with_holes_8_b', 1234567),
      ):
        with self.subTest(layer=lyr_name):
          lyr = ds.GetLayerByName(lyr_name)
          self.assertEqual(lyr.GetFeatureCount(), 1)
          feat = lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          self.assertEqual(feat.GetFID(), expected_fid)

      for lyr_name, expected_fid in (
          ('with_holes_8_c', 334),
          ('with_holes_8_d', 277),
          ('with_holes_8_e', 722),
      ):
        with self.subTest(layer=lyr_name):
          lyr = ds.GetLayerByName(lyr_name)
          gdal.ErrorReset()
          with gdal.quiet_errors():
            self.assertEqual(lyr.GetFeatureCount(), 1)
          self.assertIn(
              'Due to partial reverse engineering of the format',
              gdal.GetLastErrorMsg(),
          )
          feat = lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          self.assertEqual(feat.GetFID(), expected_fid)

      lyr_f = ds.GetLayerByName('with_holes_8_f')
      gdal.ErrorReset()
      with gdal.quiet_errors():
        self.assertEqual(lyr_f.GetFeatureCount(), 5)
      self.assertIn(
          'Due to partial reverse engineering of the format',
          gdal.GetLastErrorMsg(),
      )
      self.assertEqual(
          [feat.GetFID() for feat in lyr_f], [576, 1671, 2382, 3349, 4818]
      )

  def test_read_from_http_disabled_pinned(self) -> None:
    # Upstream: test_ogr_openfilegdb_read_from_http.
    # HAVE_CURL (/vsicurl/, HTTP driver) is disabled in Google3, so /vsicurl/ is
    # treated as an unmounted literal path rather than a network filesystem.
    self.assertIsNone(gdal.GetDriverByName('HTTP'))
    self.assertNotIn('/vsicurl/', gdal.GetFileSystemsPrefixes())
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, r'No such file or directory'),
    ):
      with self._open_gdb('/vsicurl/nonexistent.gdb.zip') as _:
        pass

  def test_arc_interior_point_bug_line(self) -> None:
    # Upstream: test_ogr_openfilegdb_arc_interior_point_bug_line
    filepath = self._test_file_path(
        'fgdb/arc_segment_interior_point_but_line.gdb.zip'
    )
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self._assert_geometry_equal(
          feat.GetGeometryRef(),
          'MULTILINESTRING ((37252520.1717 7431529.9154,'
          '38549084.9654 758964.7573))',
      )

  def test_weird_gdbindexes(self) -> None:
    # Upstream: test_ogr_openfilegdb_weird_gdbindexes
    filepath = self._test_file_path('fgdb/corrupted_gdbindexes.gdb')
    with self._open_gdb(filepath) as ds:
      lyr = ds.GetLayer(0)
      with _attribute_filter(lyr, "id = '1'"):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat['id'], '1')
        self.assertIsNone(lyr.GetNextFeature())

  def test_vsizip_random_zip_name_no_gdb_subdir(self) -> None:
    # Upstream: test_ogr_openfilegdb_vsizip_random_zip_name_and_no_gdb_subdir
    filepath = self._test_file_path('fgdb/testopenfilegdb.zip')
    with self._open_gdb(f'/vsizip/{filepath}') as ds:
      self.assertEqual(ds.GetLayerCount(), 37)
      self.assertEqual(ds.GetLayerByName('point').GetFeatureCount(), 5)


if __name__ == '__main__':
  googletest.main()
