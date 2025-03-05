/*
 * Copyright 2024 The Android Open Source Project
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

#ifndef ANDROID_C2_SOFT_APV_COMMON_H__
#define ANDROID_C2_SOFT_APV_COMMON_H__

typedef enum {
    PIX_CHROMA_NA = 0xFFFFFFFF,
    PIX_YUV_420P = 0x1,
    PIX_YUV_422P = 0x2,
    PIX_420_UV_INTL = 0x3,
    PIX_YUV_422IBE = 0x4,
    PIX_YUV_422ILE = 0x5,
    PIX_YUV_444P = 0x6,
    PIX_YUV_411P = 0x7,
    PIX_GRAY = 0x8,
    PIX_RGB_565 = 0x9,
    PIX_RGB_24 = 0xa,
    PIX_YUV_420SP_UV = 0xb,
    PIX_YUV_420SP_VU = 0xc,
    PIX_YUV_422SP_UV = 0xd,
    PIX_YUV_422SP_VU = 0xe
} PIX_COLOR_FORMAT_T;

#define CLIP_VAL(n, min, max) (((n) > (max)) ? (max) : (((n) < (min)) ? (min) : (n)))
#define ALIGN_VAL(val, align) ((((val) + (align) - 1) / (align)) * (align))

static int atomic_inc(volatile int* pcnt) {
    int ret;
    ret = *pcnt;
    ret++;
    *pcnt = ret;
    return ret;
}

static int atomic_dec(volatile int* pcnt) {
    int ret;
    ret = *pcnt;
    ret--;
    *pcnt = ret;
    return ret;
}

/* Function to allocate memory for picture buffer:
   This function might need to modify according to O/S or CPU platform
*/
static void* picbuf_alloc(int size) {
    return malloc(size);
}

/* Function to free memory allocated for picture buffer:
   This function might need to modify according to O/S or CPU platform
*/
static void picbuf_free(void* p) {
    if (p) {
        free(p);
    }
}

static int imgb_addref(oapv_imgb_t* imgb) {
    return atomic_inc(&imgb->refcnt);
}

static int imgb_getref(oapv_imgb_t* imgb) {
    return imgb->refcnt;
}

static int imgb_release(oapv_imgb_t* imgb) {
    int refcnt, i;
    refcnt = atomic_dec(&imgb->refcnt);
    if (refcnt == 0) {
        for (i = 0; i < OAPV_MAX_CC; i++) {
            if (imgb->baddr[i]) picbuf_free(imgb->baddr[i]);
        }
        free(imgb);
    }
    return refcnt;
}

static oapv_imgb_t* imgb_create(int w, int h, int cs) {
    int i, bd;
    oapv_imgb_t* imgb;

    imgb = (oapv_imgb_t*)malloc(sizeof(oapv_imgb_t));
    if (imgb == NULL) goto ERR;
    memset(imgb, 0, sizeof(oapv_imgb_t));

    bd = OAPV_CS_GET_BYTE_DEPTH(cs); /* byte unit */

    imgb->w[0] = w;
    imgb->h[0] = h;
    switch (OAPV_CS_GET_FORMAT(cs)) {
        case OAPV_CF_YCBCR400:
            imgb->w[1] = imgb->w[2] = w;
            imgb->h[1] = imgb->h[2] = h;
            imgb->np = 1;
            break;
        case OAPV_CF_YCBCR420:
            imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
            imgb->h[1] = imgb->h[2] = (h + 1) >> 1;
            imgb->np = 3;
            break;
        case OAPV_CF_YCBCR422:
            imgb->w[1] = imgb->w[2] = (w + 1) >> 1;
            imgb->h[1] = imgb->h[2] = h;
            imgb->np = 3;
            break;
        case OAPV_CF_YCBCR444:
            imgb->w[1] = imgb->w[2] = w;
            imgb->h[1] = imgb->h[2] = h;
            imgb->np = 3;
            break;
        case OAPV_CF_YCBCR4444:
            imgb->w[1] = imgb->w[2] = imgb->w[3] = w;
            imgb->h[1] = imgb->h[2] = imgb->h[3] = h;
            imgb->np = 4;
            break;
        case OAPV_CF_PLANAR2:
            imgb->w[1] = w;
            imgb->h[1] = h;
            imgb->np = 2;
            break;
        default:
            goto ERR;
    }

    for (i = 0; i < imgb->np; i++) {
        // width and height need to be aligned to macroblock size
        imgb->aw[i] = ALIGN_VAL(imgb->w[i], OAPV_MB_W);
        imgb->s[i] = imgb->aw[i] * bd;
        imgb->ah[i] = ALIGN_VAL(imgb->h[i], OAPV_MB_H);
        imgb->e[i] = imgb->ah[i];

        imgb->bsize[i] = imgb->s[i] * imgb->e[i];
        imgb->a[i] = imgb->baddr[i] = picbuf_alloc(imgb->bsize[i]);
        memset(imgb->a[i], 0, imgb->bsize[i]);
    }
    imgb->cs = cs;
    imgb->addref = imgb_addref;
    imgb->getref = imgb_getref;
    imgb->release = imgb_release;

    imgb->addref(imgb); /* increase reference count */
    return imgb;

ERR:
    if (imgb) {
        for (int i = 0; i < OAPV_MAX_CC; i++) {
            if (imgb->a[i]) picbuf_free(imgb->a[i]);
        }
        free(imgb);
    }
    return NULL;
}

#endif  // ANDROID_C2_SOFT_APV_COMMON_H__