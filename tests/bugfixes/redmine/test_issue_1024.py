# -*- coding: utf-8 -*-

import system_tests

class CheckRegularExpressionSupport(metaclass=system_tests.CaseMeta):

    url = "http://dev.exiv2.org/issues/1024"

    filename = system_tests.path("$data_path/exiv2-bug1024.exv")
    commands = [ "$exiv2 -pa --grep gpsl/i $filename" ]

    stdout = [ """Exif.GPSInfo.GPSLatitudeRef                  Ascii       2  North
Exif.GPSInfo.GPSLatitude                     Rational    3  52 deg 3' 49.02"
Exif.GPSInfo.GPSLongitudeRef                 Ascii       2  East
Exif.GPSInfo.GPSLongitude                    Rational    3  1 deg 13' 49.16"
"""
    ]
    stderr = [""]
    retval = [0]
