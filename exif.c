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
 */

#ifdef _MSC_VER
#include <windows.h>
#endif
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <ctype.h>
#include "exif.h"

#pragma pack(2)

#define VERSION  "1.0.0"

// TIFF Header
typedef struct _tiff_Header {
    unsigned short byteOrder;
    unsigned short reserved;
    unsigned int Ifd0thOffset;
} TIFF_HEADER;

// APP1 Exif Segment Header
typedef struct _App1_Header {
    unsigned short marker;
    unsigned short length;
    char id[6]; // "Exif\0\0"
    TIFF_HEADER tiff;
} APP1_HEADER;

// tag field in IFD
typedef struct {
    unsigned short tag;
    unsigned short type;
    unsigned int count;
    unsigned int offset;
} IFD_TAG;

// tag node - internal use
typedef struct _tagNode TagNode;
struct _tagNode {
    unsigned short tagId;
    unsigned short type;
    unsigned int count;
    unsigned int *numData;
    unsigned char *byteData;
    unsigned short error;
    TagNode *prev;
    TagNode *next;
};

// IFD table - internal use
typedef struct _ifdTable IfdTable;
struct _ifdTable {
    IFD_TYPE ifdType;
    unsigned short tagCount;
    TagNode *tags;
    unsigned int nextIfdOffset;
};

static int init(FILE*);
static int systemIsLittleEndian();
static int dataIsLittleEndian();
static void freeIfdTable(void*);
static void *parseIFD(FILE*, unsigned int, IFD_TYPE);
static TagNode *getTagNodePtrFromIfd(IfdTable*, unsigned short);
static TagNode *duplicateTagNode(TagNode*);
static void freeTagNode(void*);
static char *getTagName(int, unsigned short);

static int Verbose = 0;
static int App1StartOffset = -1;
static APP1_HEADER App1Header;

// public funtions

/**
 * setVerbose()
 *
 * Verbose output on/off
 *
 * parameters
 *  [in] v : 1=on  0=off
 */
void setVerbose(int v)
{
    Verbose = v;
}

/**
 * removeExifSegmentFromJPEGFile()
 *
 * Remove the Exif segment from a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *
 * return
 *   1: OK
 *   0: the Exif segment is not found
 *  -n: error
 *      ERR_READ_FILE
 *      ERR_WRITE_FILE
 *      ERR_INVALID_JPEG
 *      ERR_INVALID_APP1HEADER
 */
int removeExifSegmentFromJPEGFile(const char *inJPEGFileName,
                                  const char *outJPGEFileName)
{
    int ofs;
    int i, sts = 1;
    size_t readLen, writeLen;
    unsigned char buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fpr);
    if (sts <= 0) {
        goto DONE;
    }
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    // copy the data in front of the Exif segment
    rewind(fpr);
    p = buf;
    if (App1StartOffset > sizeof(buf)) {
        // allocate new buffer if needed
        p = (unsigned char*)malloc(App1StartOffset);
    }
    if (!p) {
        for (i = 0; i < App1StartOffset; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, App1StartOffset, fpr) < (size_t)App1StartOffset) {
            sts = ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, App1StartOffset, fpw) < (size_t)App1StartOffset) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    // seek to the end of the Exif segment
    ofs = App1StartOffset + sizeof(App1Header.marker) + App1Header.length;
    if (fseek(fpr, ofs, SEEK_SET) != 0) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}

/**
 * createIfdTableArray()
 *
 * Parse the JPEG header and create the pointer array of the IFD tables
 *
 * parameters
 *  [in] JPEGFileName : target JPEG file
 *  [out] result : result status value 
 *   n: number of IFD tables
 *   0: the Exif segment is not found
 *  -n: error
 *      ERR_READ_FILE
 *      ERR_INVALID_JPEG
 *      ERR_INVALID_APP1HEADER
 *      ERR_INVALID_IFD
 *
 * return
 *   NULL: error or no Exif segment
 *  !NULL: pointer array of the IFD tables
 */
void **createIfdTableArray(const char *JPEGFileName, int *result)
{
    #define FMT_ERR "critical error in %s IFD\n"

    int i, sts = 1, ifdCount = 0;
    unsigned int ifdOffset;
    FILE *fp = NULL;
    TagNode *tag;
    void **ppIfdArray = NULL;
    void *ifdArray[32];
    IfdTable *ifd_0th, *ifd_exif, *ifd_gps, *ifd_io, *ifd_1st;

    ifd_0th = ifd_exif = ifd_gps = ifd_io = ifd_1st = NULL;
    memset(ifdArray, 0, sizeof(ifdArray));

    fp = fopen(JPEGFileName, "rb");
    if (!fp) {
        sts = ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fp);
    if (sts <= 0) {
        goto DONE;
    }
    if (Verbose) {
        printf("system: %s-endian\n  data: %s-endian\n", 
            systemIsLittleEndian() ? "little" : "big",
            dataIsLittleEndian() ? "little" : "big");
    }

    // for 0th IFD
    ifd_0th = parseIFD(fp, App1Header.tiff.Ifd0thOffset, IFD_0TH);
    if (!ifd_0th) {
        if (Verbose) {
            printf(FMT_ERR, "0th");
        }
        sts = ERR_INVALID_IFD;
        goto DONE; // non-continuable
    }
    ifdArray[ifdCount++] = ifd_0th;

    // for Exif IFD 
    tag = getTagNodePtrFromIfd(ifd_0th, TAG_ExifIFDPointer);
    if (tag && !tag->error) {
        ifdOffset = tag->numData[0];
        ifd_exif = parseIFD(fp, ifdOffset, IFD_EXIF);
        if (ifd_exif) {
            ifdArray[ifdCount++] = ifd_exif;
            tag = getTagNodePtrFromIfd(ifd_exif, TAG_InteroperabilityIFDPointer);
            if (tag && !tag->error) {
                ifdOffset = tag->numData[0];
                ifd_io = parseIFD(fp, ifdOffset, IFD_IO);
                if (ifd_io) {
                    ifdArray[ifdCount++] = ifd_io;
                } else {
                    if (Verbose) {
                        printf(FMT_ERR, "Interoperability");
                    }
                    sts = ERR_INVALID_IFD;
                }
            }
        } else {
            if (Verbose) {
                printf(FMT_ERR, "Exif");
            }
            sts = ERR_INVALID_IFD;
        }
    }

    // for GPS IFD
    tag = getTagNodePtrFromIfd(ifd_0th, TAG_GPSInfoIFDPointer);
    if (tag && !tag->error) {
        ifdOffset = tag->numData[0];
        ifd_gps = parseIFD(fp, ifdOffset, IFD_GPS);
        if (ifd_gps) {
            ifdArray[ifdCount++] = ifd_gps;
        } else {
            if (Verbose) {
                printf(FMT_ERR, "GPS");
            }
            sts = ERR_INVALID_IFD;
        }
    }

    // for 1st IFD
    ifdOffset = ifd_0th->nextIfdOffset;
    if (ifdOffset != 0) {
        ifd_1st = parseIFD(fp, ifdOffset, IFD_1ST);
        if (ifd_1st) {
            ifdArray[ifdCount++] = ifd_1st;
        } else {
            if (Verbose) {
                printf(FMT_ERR, "1st");
            }
            sts = ERR_INVALID_IFD;
        }
    }

DONE:
    *result = (sts <= 0) ? sts : ifdCount;
    if (ifdCount > 0) {
        // +1 extra NULL element to the array 
        ppIfdArray = (void**)malloc(sizeof(void*)*(ifdCount+1));
        memset(ppIfdArray, 0, sizeof(void*)*(ifdCount+1));
        for (i = 0; ifdArray[i] != NULL; i++) {
            ppIfdArray[i] = ifdArray[i];
        }
    }
    if (fp) {
        fclose(fp);
    }
    return ppIfdArray;
}

/**
 * freeIfdTableArray()
 *
 * Free the pointer array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void freeIfdTableArray(void **ifdArray)
{
    int i;
    for (i = 0; ifdArray[i] != NULL; i++) {
        freeIfdTable(ifdArray[i]);
    }
    free(ifdArray);
}

/**
 * getIfdType()
 *
 * Returns the type of the IFD
 *
 * parameters
 *  [in] ifd: target IFD
 *
 * return
 *  IFD TYPE value
 */
IFD_TYPE getIfdType(void *pIfd)
{
    IfdTable *ifd = (IfdTable*)pIfd;
    if (!ifd) {
        return IFD_UNKNOWN;
    }
    return ifd->ifdType;
}

/**
 * dumpIfdTable()
 *
 * Dump the IFD table
 *
 * parameters
 *  [in] ifd: target IFD
 */
void dumpIfdTable(void *pIfd)
{
    int i;
    IfdTable *ifd;
    TagNode *tag;
    char tagName[512];
    int cnt = 0;
    unsigned int count;

    if (!pIfd) {
        return;
    }
    ifd = (IfdTable*)pIfd;

    printf("\n{%s IFD}",
        (ifd->ifdType == IFD_0TH)  ? "0TH" :
        (ifd->ifdType == IFD_1ST)  ? "1ST" :
        (ifd->ifdType == IFD_EXIF) ? "EXIF" :
        (ifd->ifdType == IFD_GPS)  ? "GPS" :
        (ifd->ifdType == IFD_IO)   ? "Interoperability" : "");

    if (Verbose) {
        printf(" tags=%u\n", ifd->tagCount);
    } else {
        printf("\n");
    }

    tag = ifd->tags;
    while (tag) {
        if (Verbose) {
            printf("tag[%02d] 0x%04X %s\n",
                cnt++, tag->tagId, getTagName(ifd->ifdType, tag->tagId));
            printf("\ttype=%u count=%u ", tag->type, tag->count);
            printf("val=");
        } else {
            strcpy(tagName, getTagName(ifd->ifdType, tag->tagId));
            printf(" - %s: ", (strlen(tagName) > 0) ? tagName : "(unknown)");
        }
        if (tag->error) {
            printf("(error)");
        } else {
            switch (tag->type) {
            case TYPE_BYTE:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%u ", (unsigned char)tag->numData[i]);
                }
                break;

            case TYPE_ASCII:
                printf("[%s]", (char*)tag->byteData);
                break;

            case TYPE_SHORT:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%hu ", (unsigned short)tag->numData[i]);
                }
                break;

            case TYPE_LONG:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%u ", tag->numData[i]);
                }
                break;

            case TYPE_RATIONAL:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%u/%u ", tag->numData[i*2], tag->numData[i*2+1]);
                }
                break;

            case TYPE_SBYTE:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%d ", (char)tag->numData[i]);
                }
                break;

            case TYPE_UNDEFINED:
                count = tag->count;
                // omit too long data
                if (count > 16) { // && !Verbose) {
                    count = 16;
                }
                for (i = 0; i < (int)count; i++) {
                    // if character is printable
                    if (isgraph(tag->byteData[i])) {
                        printf("%c ", tag->byteData[i]);
                    } else {
                        printf("0x%02x ", tag->byteData[i]);
                    }
                }
                if (count < tag->count) {
                    printf("(omitted)");
                }
                break;

            case TYPE_SSHORT:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%hd ", (short)tag->numData[i]);
                }
                break;

            case TYPE_SLONG:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%d ", (int)tag->numData[i]);
                }
                break;

            case TYPE_SRATIONAL:
                for (i = 0; i < (int)tag->count; i++) {
                    printf("%d/%d ", (int)tag->numData[i*2], (int)tag->numData[i*2+1]);
                }
                break;

            default:
                break;
            }
        }
        printf("\n");

        tag = tag->next;
    }
    return;
}

/**
 * dumpIfdTableArray()
 *
 * Dump the array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void dumpIfdTableArray(void **ifdArray)
{
    int i;
    if (ifdArray) {
        for (i = 0; ifdArray[i] != NULL; i++) {
            dumpIfdTable(ifdArray[i]);
        }
    }
}

/**
 * getTagInfo()
 *
 * Get the TagNodeInfo that matches the IFD_TYPE & TagId
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 *  [in] ifdType : target IFD TYPE
 *  [in] tagId : target tag ID
 *
 * return
 *   NULL: tag is not found
 *  !NULL: address of the TagNodeInfo structure
 */
TagNodeInfo *getTagInfo(void **ifdArray,
                       IFD_TYPE ifdType,
                       unsigned short tagId)
{
    int i;
    if (!ifdArray) {
        return NULL;
    }
    for (i = 0; ifdArray[i] != NULL; i++) {
        if (getIfdType(ifdArray[i]) == ifdType) {
            void *targetTag = getTagNodePtrFromIfd(ifdArray[i], tagId);
            if (!targetTag) {
                return NULL;
            }
            return (TagNodeInfo*)duplicateTagNode(targetTag);
        }
    }
    return NULL;
}

/**
 * getTagInfoFromIfd()
 *
 * Get the TagNodeInfo that matches the TagId
 *
 * parameters
 *  [in] ifd : target IFD
 *  [in] tagId : target tag ID
 *
 * return
 *  NULL: tag is not found
 *  !NULL: address of the TagNodeInfo structure
 */
TagNodeInfo *getTagInfoFromIfd(void *ifd,
                               unsigned short tagId)
{
    if (!ifd) {
        return NULL;
    }
    return (TagNodeInfo*)getTagNodePtrFromIfd(ifd, tagId);
}

/**
 * freeTagInfo()
 *
 * Free the TagNodeInfo allocated by getTagInfo() or getTagInfoFromIfd()
 *
 * parameters
 *  [in] tag : target TagNodeInfo
 */
void freeTagInfo(void *tag)
{
    freeTagNode(tag);
}


// private functions

static int dataIsLittleEndian()
{
    return (App1Header.tiff.byteOrder == 0x4949) ? 1 : 0;
}

static int systemIsLittleEndian()
{
    static int i = 1;
    return (int)(*(char*)&i);
}

static unsigned short swab16(unsigned short us)
{
    return (us << 8) | ((us >> 8) & 0x00FF);
}

static unsigned int swab32(unsigned int ui)
{
    return
    ((ui << 24) & 0xFF000000) | ((ui << 8)  & 0x00FF0000) |
    ((ui >> 8)  & 0x0000FF00) | ((ui >> 24) & 0x000000FF);
}

static unsigned short fix_short(unsigned short us)
{
    return (dataIsLittleEndian() !=
        systemIsLittleEndian()) ? swab16(us) : us;
}

static unsigned int fix_int(unsigned int ui)
{
    return (dataIsLittleEndian() !=
        systemIsLittleEndian()) ? swab32(ui) : ui;
}

static int seekToRelativeOffset(FILE *fp, unsigned int ofs)
{
    static int start = offsetof(APP1_HEADER, tiff);
    return fseek(fp, (App1StartOffset + start) + ofs, SEEK_SET);
}

static char *getTagName(int ifdType, unsigned short tagId)
{
    static char tagName[128];
    if (ifdType == IFD_0TH || ifdType == IFD_1ST || ifdType == IFD_EXIF) {
        strcpy(tagName,
            (tagId == 0x0100) ? "ImageWidth" :
            (tagId == 0x0101) ? "ImageLength" :
            (tagId == 0x0102) ? "BitsPerSample" :
            (tagId == 0x0103) ? "Compression" :
            (tagId == 0x0106) ? "PhotometricInterpretation" :
            (tagId == 0x0112) ? "Orientation" :
            (tagId == 0x0115) ? "SamplesPerPixel" :
            (tagId == 0x011C) ? "PlanarConfiguration" :
            (tagId == 0x0212) ? "YCbCrSubSampling" :
            (tagId == 0x0213) ? "YCbCrPositioning" :
            (tagId == 0x011A) ? "XResolution" :
            (tagId == 0x011B) ? "YResolution" :
            (tagId == 0x0128) ? "ResolutionUnit" :

            (tagId == 0x0111) ? "StripOffsets" :
            (tagId == 0x0116) ? "RowsPerStrip" :
            (tagId == 0x0117) ? "StripByteCounts" :
            (tagId == 0x0201) ? "JPEGInterchangeFormat" :
            (tagId == 0x0202) ? "JPEGInterchangeFormatLength" :

            (tagId == 0x012D) ? "TransferFunction" :
            (tagId == 0x013E) ? "WhitePoint" :
            (tagId == 0x013F) ? "PrimaryChromaticities" :
            (tagId == 0x0211) ? "YCbCrCoefficients" :
            (tagId == 0x0214) ? "ReferenceBlackWhite" :

            (tagId == 0x0132) ? "DateTime" :
            (tagId == 0x010E) ? "ImageDescription" :
            (tagId == 0x010F) ? "Make" :
            (tagId == 0x0110) ? "Model" :
            (tagId == 0x0131) ? "Software" :
            (tagId == 0x013B) ? "Artist" :
            (tagId == 0x8298) ? "Copyright" :
            (tagId == 0x8769) ? "ExifIFDPointer" :
            (tagId == 0x8825) ? "GPSInfoIFDPointer":
            (tagId == 0xA005) ? "InteroperabilityIFDPointer" :

            (tagId == 0x4746) ? "Rating" :

            (tagId == 0x9000) ? "ExifVersion" :
            (tagId == 0xA000) ? "FlashPixVersion" :

            (tagId == 0xA001) ? "ColorSpace" :

            (tagId == 0x9101) ? "ComponentsConfiguration" :
            (tagId == 0x9102) ? "CompressedBitsPerPixel" :
            (tagId == 0xA002) ? "PixelXDimension" :
            (tagId == 0xA003) ? "PixelYDimension" :

            (tagId == 0x927C) ? "MakerNote" :
            (tagId == 0x9286) ? "UserComment" :

            (tagId == 0xA004) ? "RelatedSoundFile" :

            (tagId == 0x9003) ? "DateTimeOriginal" :
            (tagId == 0x9004) ? "DateTimeDigitized" :
            (tagId == 0x9290) ? "SubSecTime" :
            (tagId == 0x9291) ? "SubSecTimeOriginal" :
            (tagId == 0x9292) ? "SubSecTimeDigitized" :

            (tagId == 0x829A) ? "ExposureTime" :
            (tagId == 0x829D) ? "FNumber" :
            (tagId == 0x8822) ? "ExposureProgram" :
            (tagId == 0x8824) ? "SpectralSensitivity" :
            (tagId == 0x8827) ? "PhotographicSensitivity" :
            (tagId == 0x8828) ? "OECF" :
            (tagId == 0x8830) ? "SensitivityType" :
            (tagId == 0x8831) ? "StandardOutputSensitivity" :
            (tagId == 0x8832) ? "RecommendedExposureIndex" :
            (tagId == 0x8833) ? "ISOSpeed" :
            (tagId == 0x8834) ? "ISOSpeedLatitudeyyy" :
            (tagId == 0x8835) ? "ISOSpeedLatitudezzz" :

            (tagId == 0x9201) ? "ShutterSpeedValue" :
            (tagId == 0x9202) ? "ApertureValue" :
            (tagId == 0x9203) ? "BrightnessValue" :
            (tagId == 0x9204) ? "ExposureBiasValue" :
            (tagId == 0x9205) ? "MaxApertureValue" :
            (tagId == 0x9206) ? "SubjectDistance" :
            (tagId == 0x9207) ? "MeteringMode" :
            (tagId == 0x9208) ? "LightSource" :
            (tagId == 0x9209) ? "Flash" :
            (tagId == 0x920A) ? "FocalLength" :
            (tagId == 0x9214) ? "SubjectArea" :
            (tagId == 0xA20B) ? "FlashEnergy" :
            (tagId == 0xA20C) ? "SpatialFrequencyResponse" :
            (tagId == 0xA20E) ? "FocalPlaneXResolution" :
            (tagId == 0xA20F) ? "FocalPlaneYResolution" :
            (tagId == 0xA210) ? "FocalPlaneResolutionUnit" :
            (tagId == 0xA214) ? "SubjectLocation" :
            (tagId == 0xA215) ? "ExposureIndex" :
            (tagId == 0xA217) ? "SensingMethod" :
            (tagId == 0xA300) ? "FileSource" :
            (tagId == 0xA301) ? "SceneType" :
            (tagId == 0xA302) ? "CFAPattern" :

            (tagId == 0xA401) ? "CustomRendered" :
            (tagId == 0xA402) ? "ExposureMode" :
            (tagId == 0xA403) ? "WhiteBalance" :
            (tagId == 0xA404) ? "DigitalZoomRatio" :
            (tagId == 0xA405) ? "FocalLengthIn35mmFormat" :
            (tagId == 0xA406) ? "SceneCaptureType" :
            (tagId == 0xA407) ? "GainControl" :
            (tagId == 0xA408) ? "Contrast" :
            (tagId == 0xA409) ? "Saturation" :
            (tagId == 0xA40A) ? "Sharpness" :
            (tagId == 0xA40B) ? "DeviceSettingDescription" :
            (tagId == 0xA40C) ? "SubjectDistanceRange" :

            (tagId == 0xA420) ? "ImageUniqueID" :
            (tagId == 0xA430) ? "CameraOwnerName" :
            (tagId == 0xA431) ? "BodySerialNumber" :
            (tagId == 0xA432) ? "LensSpecification" :
            (tagId == 0xA433) ? "LensMake" :
            (tagId == 0xA434) ? "LensModel" :
            (tagId == 0xA435) ? "LensSerialNumber" :
            (tagId == 0xA500) ? "Gamma" : 
            "(unknown)");
    } else if (ifdType == IFD_GPS) {
        strcpy(tagName,
            (tagId == 0x0000) ? "GPSVersionID" :
            (tagId == 0x0001) ? "GPSLatitudeRef" :
            (tagId == 0x0002) ? "GPSLatitude" :
            (tagId == 0x0003) ? "GPSLongitudeRef" :
            (tagId == 0x0004) ? "GPSLongitude" :
            (tagId == 0x0005) ? "GPSAltitudeRef" :
            (tagId == 0x0006) ? "GPSAltitude" :
            (tagId == 0x0007) ? "GPSTimeStamp" :
            (tagId == 0x0008) ? "GPSSatellites" :
            (tagId == 0x0009) ? "GPSStatus" :
            (tagId == 0x000A) ? "GPSMeasureMode" :
            (tagId == 0x000B) ? "GPSDOP" :
            (tagId == 0x000C) ? "GPSSpeedRef" :
            (tagId == 0x000D) ? "GPSSpeed" :
            (tagId == 0x000E) ? "GPSTrackRef" :
            (tagId == 0x000F) ? "GPSTrack" :
            (tagId == 0x0010) ? "GPSImgDirectionRef" :
            (tagId == 0x0011) ? "GPSImgDirection" :
            (tagId == 0x0012) ? "GPSMapDatum" :
            (tagId == 0x0013) ? "GPSDestLatitudeRef" :
            (tagId == 0x0014) ? "GPSDestLatitude" :
            (tagId == 0x0015) ? "GPSDestLongitudeRef" :
            (tagId == 0x0016) ? "GPSDestLongitude" :
            (tagId == 0x0017) ? "GPSBearingRef" :
            (tagId == 0x0018) ? "GPSBearing" :
            (tagId == 0x0019) ? "GPSDestDistanceRef" :
            (tagId == 0x001A) ? "GPSDestDistance" :
            (tagId == 0x001B) ? "GPSProcessingMethod" :
            (tagId == 0x001C) ? "GPSAreaInformation" :
            (tagId == 0x001D) ? "GPSDateStamp" :
            (tagId == 0x001E) ? "GPSDifferential" :
            (tagId == 0x001F) ? "GPSHPositioningError" :
            "(unknown)");
    } else if (ifdType == IFD_IO) {
        strcpy(tagName, 
            (tagId == 0x0001) ? "InteroperabilityIndex" :
            (tagId == 0x0002) ? "InteroperabilityVersion" :
            "(unknown)");
    }
    return tagName;
}

// create IFD table
static void *createIfdTable(IFD_TYPE IfdType, unsigned short tagCount, unsigned int nextOfs)
{
    IfdTable *ifd = (IfdTable*)malloc(sizeof(IfdTable));
    ifd->ifdType = IfdType;
    ifd->tagCount = tagCount;
    ifd->tags = NULL;
    ifd->nextIfdOffset = nextOfs;
    return ifd;
}

// add tag enrtry to the IFD table
static void *addTagNodeToIfd(void *pIfd,
                      unsigned short tagId,
                      unsigned short type,
                      unsigned int count,
                      unsigned int *numData,
                      unsigned char *byteData)
{
    int i;
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return NULL;
    }
    tag = (TagNode*)malloc(sizeof(TagNode));
    memset(tag, 0, sizeof(TagNode));
    tag->tagId = tagId;
    tag->type = type;
    tag->count = count;

    if (count > 0) {
        if (numData != NULL) {
            int num = count;
            if (type == TYPE_RATIONAL ||
                type == TYPE_SRATIONAL) {
                num *= 2;
            }
            tag->numData = (unsigned int*)malloc(sizeof(int)*num);
            for (i = 0; i < num; i++) {
                tag->numData[i] = numData[i];
            }
        } else if (byteData != NULL) {
            tag->byteData = (unsigned char*)malloc(count);
            memcpy(tag->byteData, byteData, count);
        } else {
            tag->error = 1;
        }
    } else {
        tag->error = 1;
    }
    
    // first tag
    if (!ifd->tags) {
        ifd->tags = tag;
    } else {
        TagNode *tagWk = ifd->tags;
        while (tagWk->next) {
            tagWk = tagWk->next;
        }
        tagWk->next = tag;
        tag->prev = tagWk;
    }

    return tag;
}

// create a copy of TagNode
static TagNode *duplicateTagNode(TagNode *src)
{
    TagNode *dup;
    size_t len;
    if (!src || src->count <= 0) {
        return NULL;
    }
    dup = (TagNode*)malloc(sizeof(TagNode));
    memset(dup, 0, sizeof(TagNode));
    dup->tagId = src->tagId;
    dup->type = src->type;
    dup->count = src->count;
    dup->error = src->error;
    if (src->numData) {
        len = sizeof(int) * src->count;
        if (src->type == TYPE_RATIONAL ||
            src->type == TYPE_SRATIONAL) {
            len *= 2;
        }
        dup->numData = (unsigned int*)malloc(len);
        memcpy(dup->numData, src->numData, len);
    } else if (src->byteData) {
        len = sizeof(char) * src->count;
        dup->byteData = (unsigned char*)malloc(len);
        memcpy(dup->byteData, src->byteData, len);
    }
    return dup;
}

// free TagNode
static void freeTagNode(void *pTag)
{
    TagNode *tag = (TagNode*)pTag;
    if (!tag) {
        return;
    }
    if (tag->numData) {
        free(tag->numData);
    }
    if (tag->byteData) {
        free(tag->byteData);
    }
    free(tag);
}

// free entire IFD table
static void freeIfdTable(void *pIfd)
{
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return;
    }
    tag = ifd->tags;
    free(ifd);

    if (tag) {
        while (tag->next) {
            tag = tag->next;
        }
        while (tag) {
            TagNode *tagWk = tag->prev;
            freeTagNode(tag);
            tag = tagWk;
        }
    }
    return;
}

// search the specified tag's node from the IFD table
static TagNode *getTagNodePtrFromIfd(IfdTable *ifd, unsigned short tagId)
{
    TagNode *tag;
    if (!ifd) {
        return NULL;
    }
    tag = ifd->tags;
    while (tag) {
        if (tag->tagId == tagId) {
            return tag;
        }
        tag = tag->next;
    }
    return NULL;
}

/**
 * Set the data of the IFD to the internal table
 *
 * parameters
 *  [in] fp: file pointer of opened file
 *  [in] startOffset : offset of target IFD
 *  [in] ifdType : type of the IFD
 *
 * return
 *   NULL: critical error occurred
 *  !NULL: the address of the IFD table
 */
static void *parseIFD(FILE *fp,
                      unsigned int startOffset,
                      IFD_TYPE ifdType)
{
    void *ifd;
    unsigned char buf[8192];
    unsigned short tagCount, us;
    unsigned int nextOffset = 0;
    unsigned int *array, val, allocSize;
    int size, cnt, i;
    size_t len;
    int pos;
    
    // get the count of the tags
    if (seekToRelativeOffset(fp, startOffset) != 0 ||
        fread(&tagCount, 1, sizeof(short), fp) < sizeof(short)) {
        return NULL;
    }
    tagCount = fix_short(tagCount);
    pos = ftell(fp);

    // in case of the 0th IFD, check the offset of the 1st IFD
    if (ifdType == IFD_0TH) {
        // next IFD's offset is at the tail of the segment
        if (seekToRelativeOffset(fp,
                sizeof(TIFF_HEADER) + sizeof(short) + sizeof(IFD_TAG) * tagCount) != 0 ||
            fread(&nextOffset, 1, sizeof(int), fp) < sizeof(int)) {
            return NULL;
        }
        nextOffset = fix_int(nextOffset);
        fseek(fp, pos, SEEK_SET);
    }
    // create new IFD table
    ifd = createIfdTable(ifdType, tagCount, nextOffset);

    // parse all tags
    for (cnt = 0; cnt < tagCount; cnt++) {
        IFD_TAG tag;
        unsigned char data[4];
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(&tag, 1, sizeof(tag), fp) < sizeof(tag)) {
            goto ERR;
        }
        memcpy(data, &tag.offset, 4); // keep raw data temporary
        tag.tag = fix_short(tag.tag);
        tag.type = fix_short(tag.type);
        tag.count = fix_int(tag.count);
        tag.offset = fix_int(tag.offset);
        pos = ftell(fp);

        //printf("tag=0x%04X type=%u count=%u offset=%u name=[%s]\n",
        //  tag.tag, tag.type, tag.count, tag.offset, getTagName(ifdType, tag.tag));

        if (tag.type == TYPE_ASCII ||     // ascii = the null-terminated string
            tag.type == TYPE_UNDEFINED) { // undefined = the chunk data bytes
            if (tag.count <= 4)  {
                // 4 bytes or less data is placed in the 'offset' area directly
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, data);
            } else {
                // 5 bytes or more data is placed in the value area of the IFD
                unsigned char *p = buf;
                if (tag.count > sizeof(buf)) {
                    // allocate new buffer if needed
                    if (tag.count >= App1Header.length) { // illegal
                        p = NULL;
                    } else {
                        p = (unsigned char*)malloc(tag.count);
                    }
                    if (!p) {
                        // treat as an error
                        addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                        continue;
                    }
                    memset(p, 0, tag.count);
                }
                if (seekToRelativeOffset(fp, tag.offset) != 0 ||
                    fread(p, 1, tag.count, fp) < tag.count) {
                    if (p != &buf[0]) {
                        free(p);
                    }
                    addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                    continue;
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, p);
                if (p != &buf[0]) {
                    free(p);
                }
            }
        }
        else if (tag.type == TYPE_RATIONAL || tag.type == TYPE_SRATIONAL) {
            unsigned int realCount = tag.count * 2; // need double the space
            size_t len = realCount * sizeof(int);
            if (len >= App1Header.length) { // illegal
                array = NULL;
            } else {
                array = (unsigned int*)malloc(len);
                if (array) {
                    if (seekToRelativeOffset(fp, tag.offset) != 0 ||
                        fread(array, 1, len , fp) < len) {
                        free(array);
                        array = NULL;
                    } else {
                        for (i = 0; i < (int)realCount; i++) {
                            array[i] = fix_int(array[i]);
                        }
                    }
                }
            }
            addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, array, NULL);
            if (array) {
                free(array);
            }
        }
        else if (tag.type == TYPE_BYTE   ||
                 tag.type == TYPE_SHORT  ||
                 tag.type == TYPE_LONG   ||
                 tag.type == TYPE_SBYTE  ||
                 tag.type == TYPE_SSHORT ||
                 tag.type == TYPE_SLONG ) {

            // the single value is always stored in tag.offset area directly
            // # the data is Left-justified if less than 4 bytes
            if (tag.count <= 1) {
                val = tag.offset;
                if (tag.type == TYPE_BYTE || tag.type == TYPE_SBYTE) {
                    unsigned char uc = data[0];
                    val = uc;
                } else if (tag.type == TYPE_SHORT || tag.type == TYPE_SSHORT) {
                    memcpy(&us, data, sizeof(short));
                    us = fix_short(us);
                    val = us;
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, &val, NULL);
             }
             // multiple value
             else {
                size = sizeof(int);
                if (tag.type == TYPE_BYTE || tag.type == TYPE_SBYTE) {
                    size = sizeof(char);
                } else if (tag.type == TYPE_SHORT || tag.type == TYPE_SSHORT) {
                    size = sizeof(short);
                }
                // for the sake of simplicity, using the 4bytes area for
                // each numeric data type 
                allocSize = sizeof(int) * tag.count;
                if (allocSize >= App1Header.length) { // illegal
                    array = NULL;
                } else {
                    array = (unsigned int*)malloc(allocSize);
                }
                if (!array) {
                    addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                    continue;
                }
                len = size * tag.count;
                // if the total length of the value is less than or equal to 4bytes, 
                // they have been stored in the tag.offset area
                if (len <= 4) {
                    if (size == 1) { // byte
                        for (i = 0; i < (int)tag.count; i++) {
                            array[i] = (unsigned int)data[i];
                        }
                    } else if (size == 2) { // short
                        for (i = 0; i < 2; i++) {
                            memcpy(&us, &data[i*2], sizeof(short));
                            us = fix_short(us);
                            array[i] = (unsigned int)us;
                        }
                    }
                } else {
                    if (seekToRelativeOffset(fp, tag.offset) != 0 ||
                        fread(buf, 1, len , fp) < len) {
                        addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                        continue;
                    }
                    for (i = 0; i < (int)tag.count; i++) {
                        memcpy(&val, &buf[i*size], size);
                        if (size == sizeof(int)) {
                            val = fix_int(val);
                        } else if (size == sizeof(short)) {
                            val = fix_short((unsigned short)val);
                        }
                        array[i] = (unsigned int)val;
                    }
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, array, NULL);
                free(array);
             }
         }
    }
    return ifd;
ERR:
    if (ifd) {
        freeIfdTable(ifd);
    }
    return NULL;
}

/**
 * Load the APP1 segment header
 *
 * return
 *  1: success
 *  0: error
 */
static int readApp1SegmentHeader(FILE *fp)
{
    // read the APP1 header
    if (fseek(fp, App1StartOffset, SEEK_SET) != 0 ||
        fread(&App1Header, 1, sizeof(APP1_HEADER), fp) <
                                            sizeof(APP1_HEADER)) {
        return 0;
    }
    if (systemIsLittleEndian()) {
        // the segment length value is always in big-endian order
        App1Header.length = swab16(App1Header.length);
    }
    // byte-order identifier
    if (App1Header.tiff.byteOrder != 0x4D4D && // big-endian
        App1Header.tiff.byteOrder != 0x4949) { // little-endian
        return 0;
    }
    // TIFF version number (always 0x002A)
    App1Header.tiff.reserved = fix_short(App1Header.tiff.reserved);
    if (App1Header.tiff.reserved != 0x002A) {
        return 0;
    }
    // offset of the 0TH IFD
    App1Header.tiff.Ifd0thOffset = fix_int(App1Header.tiff.Ifd0thOffset);
    return 1;
}

/**
 * Get the offset of the Exif segment in the current opened JPEG file
 *
 * return
 *   n: the offset from the beginning of the file
 *   0: the Exif segment is not found
 *  -n: error
 */
static int getApp1StartOffset(FILE *fp)
{
    #define EXIF_ID_STR     "Exif\0"
    #define EXIF_ID_STR_LEN 5

    int pos;
    unsigned char buf[64];
    unsigned short len, marker;
    if (!fp) {
        return ERR_READ_FILE;
    }
    rewind(fp);

    // check JPEG SOI Marker (0xFFD8)
    if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
        return ERR_READ_FILE;
    }
    if (systemIsLittleEndian()) {
        marker = swab16(marker);
    }
    if (marker != 0xFFD8) {
        return ERR_INVALID_JPEG;
    }
    // check for next 2 bytes
    if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
        return ERR_READ_FILE;
    }
    if (systemIsLittleEndian()) {
        marker = swab16(marker);
    }
    // if DQT marker (0xFFDB) is appeared, the application segment
    // doesn't exist
    if (marker == 0xFFDB) {
        return 0; // not found the Exif segment
    }

    pos = ftell(fp);
    for (;;) {
        // unexpected value. is not a APP[0-14] marker
        if (!(marker >= 0xFFE0 && marker <= 0xFFEF)) {
            break;
        }
        // read the length of the segment
        if (fread(&len, 1, sizeof(short), fp) < sizeof(short)) {
            return ERR_READ_FILE;
        }
        if (systemIsLittleEndian()) {
            len = swab16(len);
        }
        // if is not a APP1 segment, move to next segment
        if (marker != 0xFFE1) {
            if (fseek(fp, len - sizeof(short), SEEK_CUR) != 0) {
                return ERR_INVALID_JPEG;
            }
        } else {
            // check if it is the Exif segment
            if (fread(&buf, 1, EXIF_ID_STR_LEN, fp) < EXIF_ID_STR_LEN) {
                return ERR_READ_FILE;
            }
            if (memcmp(buf, EXIF_ID_STR, EXIF_ID_STR_LEN) == 0) {
                // return the start offset of the Exif segment
                return pos - sizeof(short);
            }
            // if is not a Exif segment, move to next segment
            if (fseek(fp, pos, SEEK_SET) != 0 ||
                fseek(fp, len, SEEK_CUR) != 0) {
                return ERR_INVALID_JPEG;
            }
        }
        // read next marker
        if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
            return ERR_READ_FILE;
        }
        if (systemIsLittleEndian()) {
            marker = swab16(marker);
        }
        pos = ftell(fp);
    }
    return 0; // not found the Exif segment
}

/**
 * Initialize
 *
 * return
 *   1: OK
 *   0: the Exif segment is not found
 *  -n: error
 */
static int init(FILE *fp)
{
    int sts;
    // get the offset of the Exif segment
    if ((sts = getApp1StartOffset(fp)) <= 0) {
        return sts;
    }
    App1StartOffset = sts;
    // Load the segment header
    if (!readApp1SegmentHeader(fp)) {
        return ERR_INVALID_APP1HEADER;
    }
    return 1;
}
