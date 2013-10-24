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

// sample functions
int sample_removeExifSegment(const char *srcJpgFileName, const char *outJpgFileName);
int sample_removeSensitiveData(const char *srcJpgFileName, const char *outJpgFileName);
int sample_queryTagExists(const char *srcJpgFileName);
int sample_updateTagData(const char *srcJpgFileName, const char *outJpgFileName);
int sample_saveThumbnail(const char *srcJpgFileName, const char *outFileName);

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


    // sample function A: remove the Exif segment in a JPEG file
    // result = sample_removeExifSegment(av[1], "removeExif.jpg");

    // sample function B: remove sensitive Exif data in a JPEG file
    // result = sample_removeSensitiveData(av[1], "removeSensitive.jpg");

    // sample function C: check if "GPSLatitude" tag exists in GPS IFD
    // result = sample_queryTagExists(av[1]);

    // sample function D: Update the value of "Make" tag in 0th IFD
    // result = sample_updateTagData(av[1], "updateTag.jpg");

    // sample function E: Write Exif thumbnail data to file
    // result = sample_saveThumbnail(av[1], "thumbnail.jpg");

    return result;
}

/**
 * sample_removeExifSegment()
 *
 * remove the Exif segment in a JPEG file
 *
 */
int sample_removeExifSegment(const char *srcJpgFileName, const char *outJpgFileName)
{
    int sts = removeExifSegmentFromJPEGFile(srcJpgFileName, outJpgFileName);
    if (sts <= 0) {
        printf("removeExifSegmentFromJPEGFile: ret=%d\n", sts);
    }
    return sts;
}

/**
 * sample_removeSensitiveData()
 *
 * remove sensitive Exif data in a JPEG file
 *
 */
int sample_removeSensitiveData(const char *srcJpgFileName, const char *outJpgFileName)
{
    int sts, result;
    void **ifdTableArray = createIfdTableArray(srcJpgFileName, &result);

    if (!ifdTableArray) {
        printf("createIfdTableArray: ret=%d\n", result);
        return result;
    }

    // remove GPS IFD and 1st IFD if exist
    removeIfdTableFromIfdTableArray(ifdTableArray, IFD_GPS);
    removeIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    
    // remove tags if exist
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_Make);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_Model);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_DateTime);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_ImageDescription);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_Software);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_Artist);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_MakerNote);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_UserComment);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_DateTimeOriginal);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_DateTimeDigitized);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_SubSecTime);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_SubSecTimeOriginal);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_SubSecTimeDigitized);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_ImageUniqueID);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_CameraOwnerName);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_BodySerialNumber);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_LensMake);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_LensModel);
    removeTagNodeFromIfdTableArray(ifdTableArray, IFD_EXIF, TAG_LensSerialNumber);
    
    // update the Exif segment
    sts = updateExifSegmentInJPEGFile(srcJpgFileName, outJpgFileName, ifdTableArray);
    if (sts < 0) {
        printf("updateExifSegmentInJPEGFile: ret=%d\n", sts);
    }
    freeIfdTableArray(ifdTableArray);
    return sts;
}

/**
 * sample_queryTagExists()
 *
 * check if "GPSLatitude" tag exists in GPS IFD
 *
 */
int sample_queryTagExists(const char *srcJpgFileName)
{
    int sts, result;
    void **ifdTableArray = createIfdTableArray(srcJpgFileName, &result);
    if (!ifdTableArray) {
        printf("createIfdTableArray: ret=%d\n", result);
        return result;
    }

    sts = queryTagNodeIsExist(ifdTableArray, IFD_GPS, TAG_GPSLatitude);
    printf("GPSLatitude tag is %s in [%s]\n", (sts) ? "exists" : "not exists", srcJpgFileName);

    freeIfdTableArray(ifdTableArray);
    return sts;
}

/**
 * sample_updateTagData()
 *
 * Update the value of "Make" tag in 0th IFD
 *
 */
int sample_updateTagData(const char *srcJpgFileName, const char *outJpgFileName)
{
    TagNodeInfo *tag;
    int sts, result;
    void **ifdTableArray = createIfdTableArray(srcJpgFileName, &result);

    if (ifdTableArray != NULL) {
        if (queryTagNodeIsExist(ifdTableArray, IFD_0TH, TAG_Make)) {
            removeTagNodeFromIfdTableArray(ifdTableArray, IFD_0TH, TAG_Make);
        }
    } else { // Exif segment not exists
        // create new IFD table
        ifdTableArray = insertIfdTableToIfdTableArray(NULL, IFD_0TH, &result);
        if (!ifdTableArray) {
            printf("insertIfdTableToIfdTableArray: ret=%d\n", result);
            return 0;
        }
    }
    // create a tag info
    tag = createTagInfo(TAG_Make, TYPE_ASCII, 6, &result);
    if (!tag) {
        printf("createTagInfo: ret=%d\n", result);
        freeIfdTableArray(ifdTableArray);
        return result;
    }
    // set tag data
    strcpy((char*)tag->byteData, "ABCDE");
    // insert to IFD table
    insertTagNodeToIfdTableArray(ifdTableArray, IFD_0TH, tag);
    freeTagInfo(tag);

    // write file
    sts = updateExifSegmentInJPEGFile(srcJpgFileName, outJpgFileName, ifdTableArray);

    if (sts < 0) {
        printf("updateExifSegmentInJPEGFile: ret=%d\n", sts);
    }
    freeIfdTableArray(ifdTableArray);
    return sts;
}

/**
 * sample_saveThumbnail()
 *
 * Write Exif thumbnail data to file
 *
 */
int sample_saveThumbnail(const char *srcJpgFileName, const char *outFileName)
{
    unsigned char *p;
    unsigned int len;
    FILE *fp;
    int result;

    void **ifdTableArray = createIfdTableArray(srcJpgFileName, &result);
    if (!ifdTableArray) {
        printf("createIfdTableArray: ret=%d\n", result);
        return result;
    }

    // try to get thumbnail data from 1st IFD
    p = getThumbnailDataOnIfdTableArray(ifdTableArray, &len, &result);
    if (!p) {
        printf("getThumbnailDataOnIfdTableArray: ret=%d\n", result);
        freeIfdTableArray(ifdTableArray);
        return result;
    }
    // save thumbnail
    fp = fopen(outFileName, "wb");
    if (!fp) {
        printf("failed to create [%s]\n", outFileName);
        freeIfdTableArray(ifdTableArray);
        return 0;
    }
    fwrite(p, 1, len, fp);
    fclose(fp);

    free(p);
    freeIfdTableArray(ifdTableArray);
    return 0;
}
