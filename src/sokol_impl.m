// Sokol implementation unit (compiled as Objective-C for the Metal backend),
// plus a small Metal readback helper used for screenshots.
#define SOKOL_IMPL
#define SOKOL_METAL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "sokol_debugtext.h"

#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>

// Loads an image file (PNG, JPEG, ...) via ImageIO into tightly packed RGBA8 rows
// (top row first). Images larger than max_dim are scaled down. Free with free().
bool highway_load_image(const char* path, int max_dim, int* out_w, int* out_h, uint8_t** out_rgba) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
    if (!url) {
        return false;
    }
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) {
        return false;
    }
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) {
        return false;
    }
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (max_dim > 0 && (w > (size_t)max_dim || h > (size_t)max_dim)) {
        double s = (double)max_dim / (double)(w > h ? w : h);
        w = (size_t)((double)w * s);
        h = (size_t)((double)h * s);
    }
    uint8_t* px = (uint8_t*)calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
    if (ctx) {
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    if (!ctx) {
        free(px);
        return false;
    }
    *out_w = (int)w;
    *out_h = (int)h;
    *out_rgba = px;
    return true;
}

// Copies a render target image to the CPU (must be called after sg_commit() so the
// frame's command buffer is already enqueued) and writes it as PNG.
bool highway_save_png(sg_image img, int w, int h, bool bgra, const char* path) {
    sg_mtl_image_info info = sg_mtl_query_image_info(img);
    id<MTLTexture> tex = (__bridge id<MTLTexture>)info.tex[info.active_slot];
    id<MTLDevice> dev = (__bridge id<MTLDevice>)sg_mtl_device();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
    if (!tex || !dev || !queue) {
        return false;
    }
    NSUInteger bpr = (NSUInteger)w * 4;
    id<MTLBuffer> buf = [dev newBufferWithLength:bpr * (NSUInteger)h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                 toBuffer:buf
        destinationOffset:0
   destinationBytesPerRow:bpr
 destinationBytesPerImage:bpr * (NSUInteger)h];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    uint8_t* px = (uint8_t*)buf.contents;
    for (int i = 0; i < w * h; i++) {
        if (bgra) {
            uint8_t t = px[i * 4 + 0];
            px[i * 4 + 0] = px[i * 4 + 2];
            px[i * 4 + 2] = t;
        }
        px[i * 4 + 3] = 255;
    }

    bool ok = false;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, px, bpr * (size_t)h, NULL);
    CGImageRef cgimg = CGImageCreate((size_t)w, (size_t)h, 8, 32, bpr, cs,
                                     kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault,
                                     prov, NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (dst && cgimg) {
        CGImageDestinationAddImage(dst, cgimg, NULL);
        ok = CGImageDestinationFinalize(dst);
    }
    if (dst) CFRelease(dst);
    if (url) CFRelease(url);
    if (cgimg) CGImageRelease(cgimg);
    CGDataProviderRelease(prov);
    CGColorSpaceRelease(cs);
    return ok;
}
