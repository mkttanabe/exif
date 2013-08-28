/*
 * Copyright (C) 2013 KLab Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * gcc:
 * gcc -o exif sample_main.c exif.c
 *
 * Microsoft Visual C++:
 * cl.exe /o exif sample_main.c exif.c
 */

#ifdef _MSC_VER
#include <windows.h>
#endif
#include <stdio.h>

#include "exif.h"

// sample
int main(int ac, char *av[])
{
    void **ifdArray;
    TagNodeInfo *tag;
    int i, result;

#ifdef _MSC_VER
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|_CRTDBG_LEAK_CHECK_DF);
#endif
#endif

    if (ac < 2) {
        printf("usage: %s <JPEG FileName> [-v]erbose\n", av[0]);
        return 0;
    }

    // -v option
    if (ac >= 3) {
        if ((*av[2] == '-' || *av[2] == '/') && (*(av[2]+1) == 'v')) {
            setVerbose(1);
        }
    }

    // parse the JPEG header and create the pointer array of the IFD tables
    ifdArray = createIfdTableArray(av[1], &result);

    // check result status
    switch (result) {
    case 0: // no IFDs
        printf("[%s] does not seem to contain the Exif segment.\n", av[1]);
        break;
    case ERR_READ_FILE:
        printf("failed to open or read [%s].\n", av[1]);
        break;
    case ERR_INVALID_JPEG:
        printf("[%s] is not a valid JPEG file.\n", av[1]);
        break;
    case ERR_INVALID_APP1HEADER:
        printf("[%s] does not have valid Exif segment header.\n", av[1]);
        break;
    case ERR_INVALID_IFD:
        printf("[%s] contains one or more IFD errors. use -v for details.\n", av[1]);
        break;
    default:
        printf("[%s] createIfdTableArray: result=%d\n", av[1], result);
        break;
    }

    if (!ifdArray) {
        return 0;
    }

    // dump all IFD tables
    for (i = 0; ifdArray[i] != NULL; i++) {
        dumpIfdTable(ifdArray[i]);
    }
    // or dumpIfdTableArray(ifdArray);

    printf("\n");

    // get [Model] tag value from 0th IFD
    tag = getTagInfo(ifdArray, IFD_0TH, TAG_Model);
    if (tag) {
        if (!tag->error) {
            printf("0th IFD : Model = [%s]\n", tag->byteData);
        }
        freeTagInfo(tag);
    }

    // get [DateTimeOriginal] tag value from Exif IFD
    tag = getTagInfo(ifdArray, IFD_EXIF, TAG_DateTimeOriginal);
    if (tag) {
        if (!tag->error) {
            printf("Exif IFD : DateTimeOriginal = [%s]\n", tag->byteData);
        }
        freeTagInfo(tag);
    }

    // get [GPSLatitude] tag value from GPS IFD
    tag = getTagInfo(ifdArray, IFD_GPS, TAG_GPSLatitude);
    if (tag) {
        if (!tag->error) {
            printf("GPS IFD : GPSLatitude = ");
            for (i = 0; i < (int)tag->count*2; i+=2) {
                printf("%u/%u ", tag->numData[i], tag->numData[i+1]);
            }
            printf("\n");
        }
        freeTagInfo(tag);
    }

    // free IFD table array
    freeIfdTableArray(ifdArray);

    // remove the Exif segment from a JPEG file
    result = removeExifSegmentFromJPEGFile(av[1], "_noexif.jpg");
    printf("removeExifSegmentFromJPEGFile: result=%d\n", result);

    return 0;
}
