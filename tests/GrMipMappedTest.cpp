/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkTypes.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPoint.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "src/gpu/GrBackendTextureImageGenerator.h"
#include "src/gpu/GrContextPriv.h"
#include "src/gpu/GrDrawingManager.h"
#include "src/gpu/GrGpu.h"
#include "src/gpu/GrProxyProvider.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/GrSemaphore.h"
#include "src/gpu/GrSurfaceProxyPriv.h"
#include "src/gpu/GrTexture.h"
#include "src/gpu/GrTextureProxy.h"
#include "src/gpu/SkGpuDevice.h"
#include "src/image/SkImage_Base.h"
#include "src/image/SkSurface_Gpu.h"
#include "tests/Test.h"
#include "tests/TestUtils.h"

static constexpr int kSize = 8;

// Test that the correct mip map states are on the GrTextures when wrapping GrBackendTextures in
// SkImages and SkSurfaces
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrWrappedMipMappedTest, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();
    if (!dContext->priv().caps()->mipmapSupport()) {
        return;
    }

    for (auto mipMapped : {GrMipmapped::kNo, GrMipmapped::kYes}) {
        for (auto renderable : {GrRenderable::kNo, GrRenderable::kYes}) {
            // createBackendTexture currently doesn't support uploading data to mip maps
            // so we don't send any. However, we pretend there is data for the checks below which is
            // fine since we are never actually using these textures for any work on the gpu.
            GrBackendTexture backendTex;
            CreateBackendTexture(dContext, &backendTex, kSize, kSize, kRGBA_8888_SkColorType,
                                 SkColors::kTransparent, mipMapped, renderable);

            sk_sp<GrTextureProxy> proxy;
            sk_sp<SkImage> image;
            if (GrRenderable::kYes == renderable) {
                sk_sp<SkSurface> surface = SkSurface::MakeFromBackendTexture(
                                                                           dContext,
                                                                           backendTex,
                                                                           kTopLeft_GrSurfaceOrigin,
                                                                           0,
                                                                           kRGBA_8888_SkColorType,
                                                                           nullptr,
                                                                           nullptr);

                SkGpuDevice* device = ((SkSurface_Gpu*)surface.get())->getDevice();
                proxy = device->accessRenderTargetContext()->asTextureProxyRef();
            } else {
                image = SkImage::MakeFromTexture(dContext, backendTex,
                                                 kTopLeft_GrSurfaceOrigin,
                                                 kRGBA_8888_SkColorType,
                                                 kPremul_SkAlphaType, nullptr,
                                                 nullptr, nullptr);
                const GrSurfaceProxyView* view = as_IB(image)->view(dContext);
                REPORTER_ASSERT(reporter, view);
                if (!view) {
                    dContext->deleteBackendTexture(backendTex);
                    return;
                }
                proxy = view->asTextureProxyRef();
            }
            REPORTER_ASSERT(reporter, proxy);
            if (!proxy) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            REPORTER_ASSERT(reporter, proxy->isInstantiated());

            GrTexture* texture = proxy->peekTexture();
            REPORTER_ASSERT(reporter, texture);
            if (!texture) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            if (GrMipmapped::kYes == mipMapped) {
                REPORTER_ASSERT(reporter, GrMipmapped::kYes == texture->mipmapped());
                if (GrRenderable::kYes == renderable) {
                    REPORTER_ASSERT(reporter, texture->mipmapsAreDirty());
                } else {
                    REPORTER_ASSERT(reporter, !texture->mipmapsAreDirty());
                }
            } else {
                REPORTER_ASSERT(reporter, GrMipmapped::kNo == texture->mipmapped());
            }
            dContext->deleteBackendTexture(backendTex);
        }
    }
}

// Test that we correctly copy or don't copy GrBackendTextures in the GrBackendTextureImageGenerator
// based on if we will use mips in the draw and the mip status of the GrBackendTexture.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrBackendTextureImageMipMappedTest, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();
    if (!dContext->priv().caps()->mipmapSupport()) {
        return;
    }

    for (auto betMipMapped : {GrMipmapped::kNo, GrMipmapped::kYes}) {
        for (auto requestMipMapped : {GrMipmapped::kNo, GrMipmapped::kYes}) {
            GrBackendTexture backendTex;
            CreateBackendTexture(dContext, &backendTex, kSize, kSize, kRGBA_8888_SkColorType,
                                 SkColors::kTransparent, betMipMapped, GrRenderable::kNo);

            sk_sp<SkImage> image = SkImage::MakeFromTexture(dContext, backendTex,
                                                            kTopLeft_GrSurfaceOrigin,
                                                            kRGBA_8888_SkColorType,
                                                            kPremul_SkAlphaType, nullptr,
                                                            nullptr, nullptr);

            GrTextureProxy* proxy = as_IB(image)->peekProxy();
            REPORTER_ASSERT(reporter, proxy);
            if (!proxy) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            REPORTER_ASSERT(reporter, proxy->isInstantiated());

            sk_sp<GrTexture> texture = sk_ref_sp(proxy->peekTexture());
            REPORTER_ASSERT(reporter, texture);
            if (!texture) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            std::unique_ptr<SkImageGenerator> imageGen = GrBackendTextureImageGenerator::Make(
                    texture, kTopLeft_GrSurfaceOrigin, nullptr, kRGBA_8888_SkColorType,
                    kPremul_SkAlphaType, nullptr);
            REPORTER_ASSERT(reporter, imageGen);
            if (!imageGen) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            SkIPoint origin = SkIPoint::Make(0,0);
            SkImageInfo imageInfo = SkImageInfo::Make(kSize, kSize, kRGBA_8888_SkColorType,
                                                      kPremul_SkAlphaType);
            GrSurfaceProxyView genView = imageGen->generateTexture(
                    dContext, imageInfo, origin, requestMipMapped, GrImageTexGenPolicy::kDraw);
            GrSurfaceProxy* genProxy = genView.proxy();

            REPORTER_ASSERT(reporter, genProxy);
            if (!genProxy) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            if (genProxy->isLazy()) {
                genProxy->priv().doLazyInstantiation(dContext->priv().resourceProvider());
            } else if (!genProxy->isInstantiated()) {
                genProxy->instantiate(dContext->priv().resourceProvider());
            }

            REPORTER_ASSERT(reporter, genProxy->isInstantiated());
            if (!genProxy->isInstantiated()) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            GrTexture* genTexture = genProxy->peekTexture();
            REPORTER_ASSERT(reporter, genTexture);
            if (!genTexture) {
                dContext->deleteBackendTexture(backendTex);
                return;
            }

            GrBackendTexture genBackendTex = genTexture->getBackendTexture();

            if (GrBackendApi::kOpenGL == genBackendTex.backend()) {
                GrGLTextureInfo genTexInfo;
                GrGLTextureInfo origTexInfo;
                if (genBackendTex.getGLTextureInfo(&genTexInfo) &&
                    backendTex.getGLTextureInfo(&origTexInfo)) {
                    if (requestMipMapped == GrMipmapped::kYes && betMipMapped == GrMipmapped::kNo) {
                        // We did a copy so the texture IDs should be different
                        REPORTER_ASSERT(reporter, origTexInfo.fID != genTexInfo.fID);
                    } else {
                        REPORTER_ASSERT(reporter, origTexInfo.fID == genTexInfo.fID);
                    }
                } else {
                    ERRORF(reporter, "Failed to get GrGLTextureInfo");
                }
#ifdef SK_VULKAN
            } else if (GrBackendApi::kVulkan == genBackendTex.backend()) {
                GrVkImageInfo genImageInfo;
                GrVkImageInfo origImageInfo;
                if (genBackendTex.getVkImageInfo(&genImageInfo) &&
                    backendTex.getVkImageInfo(&origImageInfo)) {
                    if (requestMipMapped == GrMipmapped::kYes && betMipMapped == GrMipmapped::kNo) {
                        // We did a copy so the texture IDs should be different
                        REPORTER_ASSERT(reporter, origImageInfo.fImage != genImageInfo.fImage);
                    } else {
                        REPORTER_ASSERT(reporter, origImageInfo.fImage == genImageInfo.fImage);
                    }
                } else {
                    ERRORF(reporter, "Failed to get GrVkImageInfo");
                }
#endif
#ifdef SK_METAL
            } else if (GrBackendApi::kMetal == genBackendTex.backend()) {
                GrMtlTextureInfo genImageInfo;
                GrMtlTextureInfo origImageInfo;
                if (genBackendTex.getMtlTextureInfo(&genImageInfo) &&
                    backendTex.getMtlTextureInfo(&origImageInfo)) {
                    if (requestMipMapped == GrMipmapped::kYes && betMipMapped == GrMipmapped::kNo) {
                        // We did a copy so the texture IDs should be different
                        REPORTER_ASSERT(reporter, origImageInfo.fTexture != genImageInfo.fTexture);
                    } else {
                        REPORTER_ASSERT(reporter, origImageInfo.fTexture == genImageInfo.fTexture);
                    }
                } else {
                    ERRORF(reporter, "Failed to get GrMtlTextureInfo");
                }
#endif
#ifdef SK_DAWN
            } else if (GrBackendApi::kDawn == genBackendTex.backend()) {
                GrDawnTextureInfo genImageInfo;
                GrDawnTextureInfo origImageInfo;
                if (genBackendTex.getDawnTextureInfo(&genImageInfo) &&
                    backendTex.getDawnTextureInfo(&origImageInfo)) {
                    if (requestMipMapped == GrMipmapped::kYes && betMipMapped == GrMipmapped::kNo) {
                        // We did a copy so the texture IDs should be different
                        REPORTER_ASSERT(reporter,
                            origImageInfo.fTexture.Get() != genImageInfo.fTexture.Get());
                    } else {
                        REPORTER_ASSERT(reporter,
                            origImageInfo.fTexture.Get() == genImageInfo.fTexture.Get());
                    }
                } else {
                    ERRORF(reporter, "Failed to get GrDawnTextureInfo");
                }
#endif
            } else {
                REPORTER_ASSERT(reporter, false);
            }

            // Must make sure the uses of the backend texture have finished (we possibly have a
            // queued up copy) before we delete the backend texture.
            dContext->flushAndSubmit();

            dContext->priv().getGpu()->testingOnly_flushGpuAndSync();

            dContext->deleteBackendTexture(backendTex);
        }
    }
}

// Test that when we call makeImageSnapshot on an SkSurface we retains the same mip status as the
// resource we took the snapshot of.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrImageSnapshotMipMappedTest, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();
    if (!dContext->priv().caps()->mipmapSupport()) {
        return;
    }

    auto resourceProvider = dContext->priv().resourceProvider();

    for (auto willUseMips : {false, true}) {
        for (auto isWrapped : {false, true}) {
            GrMipmapped mipMapped = willUseMips ? GrMipmapped::kYes : GrMipmapped::kNo;
            sk_sp<SkSurface> surface;
            GrBackendTexture backendTex;
            CreateBackendTexture(dContext, &backendTex, kSize, kSize, kRGBA_8888_SkColorType,
                                 SkColors::kTransparent, mipMapped, GrRenderable::kYes);
            if (isWrapped) {
                surface = SkSurface::MakeFromBackendTexture(dContext,
                                                            backendTex,
                                                            kTopLeft_GrSurfaceOrigin,
                                                            0,
                                                            kRGBA_8888_SkColorType,
                                                            nullptr,
                                                            nullptr);
            } else {
                SkImageInfo info = SkImageInfo::Make(kSize, kSize, kRGBA_8888_SkColorType,
                                                     kPremul_SkAlphaType);
                surface = SkSurface::MakeRenderTarget(dContext, SkBudgeted::kYes, info, 0,
                                                      kTopLeft_GrSurfaceOrigin, nullptr,
                                                      willUseMips);
            }
            REPORTER_ASSERT(reporter, surface);
            if (!surface) {
                dContext->deleteBackendTexture(backendTex);
            }
            SkGpuDevice* device = ((SkSurface_Gpu*)surface.get())->getDevice();
            GrTextureProxy* texProxy = device->accessRenderTargetContext()->asTextureProxy();
            REPORTER_ASSERT(reporter, mipMapped == texProxy->mipmapped());

            texProxy->instantiate(resourceProvider);
            GrTexture* texture = texProxy->peekTexture();
            REPORTER_ASSERT(reporter, mipMapped == texture->mipmapped());

            sk_sp<SkImage> image = surface->makeImageSnapshot();
            REPORTER_ASSERT(reporter, image);
            if (!image) {
                dContext->deleteBackendTexture(backendTex);
            }
            texProxy = as_IB(image)->peekProxy();
            REPORTER_ASSERT(reporter, mipMapped == texProxy->mipmapped());

            texProxy->instantiate(resourceProvider);
            texture = texProxy->peekTexture();
            REPORTER_ASSERT(reporter, mipMapped == texture->mipmapped());

            // Must flush the dContext to make sure all the cmds (copies, etc.) from above are sent
            // to the gpu before we delete the backendHandle.
            dContext->flushAndSubmit();
            dContext->priv().getGpu()->testingOnly_flushGpuAndSync();
            dContext->deleteBackendTexture(backendTex);
        }
    }
}

// Test that we don't create a mip mapped texture if the size is 1x1 even if the filter mode is set
// to use mips. This test passes by not crashing or hitting asserts in code.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(Gr1x1TextureMipMappedTest, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();
    if (!dContext->priv().caps()->mipmapSupport()) {
        return;
    }

    // Make surface to draw into
    SkImageInfo info = SkImageInfo::MakeN32(16, 16, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurface::MakeRenderTarget(dContext, SkBudgeted::kNo, info);

    // Make 1x1 raster bitmap
    SkBitmap bmp;
    bmp.allocN32Pixels(1, 1);
    SkPMColor* pixel = reinterpret_cast<SkPMColor*>(bmp.getPixels());
    *pixel = 0;

    sk_sp<SkImage> bmpImage = SkImage::MakeFromBitmap(bmp);

    // Make sure we scale so we don't optimize out the use of mips.
    surface->getCanvas()->scale(0.5f, 0.5f);

    SkPaint paint;
    // This should upload the image to a non mipped GrTextureProxy.
    surface->getCanvas()->drawImage(bmpImage, 0, 0, &paint);
    surface->flushAndSubmit();

    // Now set the filter quality to high so we use mip maps. We should find the non mipped texture
    // in the cache for the SkImage. Since the texture is 1x1 we should just use that texture
    // instead of trying to do a copy to a mipped texture.
    paint.setFilterQuality(kHigh_SkFilterQuality);
    surface->getCanvas()->drawImage(bmpImage, 0, 0, &paint);
    surface->flushAndSubmit();
}

// Create a new render target and draw 'mipmapView' into it using the provided 'filter'.
static std::unique_ptr<GrRenderTargetContext> draw_mipmap_into_new_render_target(
        GrRecordingContext* rContext,
        GrColorType colorType,
        SkAlphaType alphaType,
        GrSurfaceProxyView mipmapView,
        GrSamplerState::MipmapMode mm) {
    auto proxyProvider = rContext->priv().proxyProvider();
    sk_sp<GrSurfaceProxy> renderTarget =
            proxyProvider->createProxy(mipmapView.proxy()->backendFormat(),
                                       {1, 1},
                                       GrRenderable::kYes,
                                       1,
                                       GrMipmapped::kNo,
                                       SkBackingFit::kApprox,
                                       SkBudgeted::kYes,
                                       GrProtected::kNo);

    auto rtc = GrRenderTargetContext::Make(rContext,
                                           colorType,
                                           nullptr,
                                           std::move(renderTarget),
                                           kTopLeft_GrSurfaceOrigin,
                                           nullptr);

    rtc->drawTexture(nullptr,
                     std::move(mipmapView),
                     alphaType,
                     GrSamplerState::Filter::kLinear,
                     mm,
                     SkBlendMode::kSrcOver,
                     {1, 1, 1, 1},
                     SkRect::MakeWH(4, 4),
                     SkRect::MakeWH(1, 1),
                     GrAA::kYes,
                     GrQuadAAFlags::kAll,
                     SkCanvas::kFast_SrcRectConstraint,
                     SkMatrix::I(),
                     nullptr);
    return rtc;
}

// Test that two opsTasks using the same mipmaps both depend on the same GrTextureResolveRenderTask.
DEF_GPUTEST(GrManyDependentsMipMappedTest, reporter, /* options */) {
    using Enable = GrContextOptions::Enable;
    using MipmapMode = GrSamplerState::MipmapMode;

    for (auto enableSortingAndReduction : {Enable::kYes, Enable::kNo}) {
        GrMockOptions mockOptions;
        mockOptions.fMipmapSupport = true;
        GrContextOptions ctxOptions;
        ctxOptions.fReduceOpsTaskSplitting = enableSortingAndReduction;
        sk_sp<GrDirectContext> dContext = GrDirectContext::MakeMock(&mockOptions, ctxOptions);
        GrDrawingManager* drawingManager = dContext->priv().drawingManager();
        if (!dContext) {
            ERRORF(reporter, "could not create mock dContext with fReduceOpsTaskSplitting %s.",
                   (Enable::kYes == enableSortingAndReduction) ? "enabled" : "disabled");
            continue;
        }

        SkASSERT(dContext->priv().caps()->mipmapSupport());

        GrBackendFormat format = dContext->defaultBackendFormat(
                kRGBA_8888_SkColorType, GrRenderable::kYes);
        GrColorType colorType = GrColorType::kRGBA_8888;
        SkAlphaType alphaType = kPremul_SkAlphaType;

        GrProxyProvider* proxyProvider = dContext->priv().proxyProvider();

        // Create a mipmapped render target.

        sk_sp<GrTextureProxy> mipmapProxy = proxyProvider->createProxy(
                format, {4, 4}, GrRenderable::kYes, 1, GrMipmapped::kYes, SkBackingFit::kExact,
                SkBudgeted::kYes, GrProtected::kNo);

        // Mark the mipmaps clean to ensure things still work properly when they won't be marked
        // dirty again until GrRenderTask::makeClosed().
        mipmapProxy->markMipmapsClean();

        auto mipmapRTC = GrRenderTargetContext::Make(
            dContext.get(), colorType, nullptr, mipmapProxy, kTopLeft_GrSurfaceOrigin, nullptr);

        mipmapRTC->clear({.1f,.2f,.3f,.4f});
        REPORTER_ASSERT(reporter, drawingManager->getLastRenderTask(mipmapProxy.get()));
        // mipmapProxy's last render task should now just be the opsTask containing the clear.
        REPORTER_ASSERT(reporter,
                mipmapRTC->testingOnly_PeekLastOpsTask() ==
                        drawingManager->getLastRenderTask(mipmapProxy.get()));

        // Mipmaps don't get marked dirty until makeClosed().
        REPORTER_ASSERT(reporter, !mipmapProxy->mipmapsAreDirty());

        GrSwizzle swizzle = dContext->priv().caps()->getReadSwizzle(format, colorType);
        GrSurfaceProxyView mipmapView(mipmapProxy, kTopLeft_GrSurfaceOrigin, swizzle);

        // Draw the dirty mipmap texture into a render target.
        auto rtc1 = draw_mipmap_into_new_render_target(dContext.get(), colorType, alphaType,
                                                       mipmapView, MipmapMode::kLinear);
        auto rtc1Task = sk_ref_sp(rtc1->testingOnly_PeekLastOpsTask());

        // Mipmaps should have gotten marked dirty during makeClosed, then marked clean again as
        // soon as a GrTextureResolveRenderTask was inserted. The way we know they were resolved is
        // if mipmapProxy->getLastRenderTask() has switched from the opsTask that drew to it, to the
        // task that resolved its mips.
        GrRenderTask* initialMipmapRegenTask = drawingManager->getLastRenderTask(mipmapProxy.get());
        REPORTER_ASSERT(reporter, initialMipmapRegenTask);
        REPORTER_ASSERT(reporter,
                initialMipmapRegenTask != mipmapRTC->testingOnly_PeekLastOpsTask());
        REPORTER_ASSERT(reporter, !mipmapProxy->mipmapsAreDirty());

        // Draw the now-clean mipmap texture into a second target.
        auto rtc2 = draw_mipmap_into_new_render_target(dContext.get(), colorType, alphaType,
                                                       mipmapView, MipmapMode::kLinear);
        auto rtc2Task = sk_ref_sp(rtc2->testingOnly_PeekLastOpsTask());

        // Make sure the mipmap texture still has the same regen task.
        REPORTER_ASSERT(reporter,
                    drawingManager->getLastRenderTask(mipmapProxy.get()) == initialMipmapRegenTask);
        SkASSERT(!mipmapProxy->mipmapsAreDirty());

        // Reset everything so we can go again, this time with the first draw not mipmapped.
        dContext->flushAndSubmit();

        // Mip regen tasks don't get added as dependencies until makeClosed().
        REPORTER_ASSERT(reporter, rtc1Task->dependsOn(initialMipmapRegenTask));
        REPORTER_ASSERT(reporter, rtc2Task->dependsOn(initialMipmapRegenTask));

        // Render something to dirty the mips.
        mipmapRTC->clear({.1f,.2f,.3f,.4f});
        auto mipmapRTCTask = sk_ref_sp(mipmapRTC->testingOnly_PeekLastOpsTask());
        REPORTER_ASSERT(reporter, mipmapRTCTask);

        // mipmapProxy's last render task should now just be the opsTask containing the clear.
        REPORTER_ASSERT(reporter,
                    mipmapRTCTask.get() == drawingManager->getLastRenderTask(mipmapProxy.get()));

        // Mipmaps don't get marked dirty until makeClosed().
        REPORTER_ASSERT(reporter, !mipmapProxy->mipmapsAreDirty());

        // Draw the dirty mipmap texture into a render target, but don't do mipmap filtering.
        rtc1 = draw_mipmap_into_new_render_target(dContext.get(), colorType, alphaType,
                                                  mipmapView, MipmapMode::kNone);

        // Mipmaps should have gotten marked dirty during makeClosed() when adding the dependency.
        // Since the last draw did not use mips, they will not have been regenerated and should
        // therefore still be dirty.
        REPORTER_ASSERT(reporter, mipmapProxy->mipmapsAreDirty());

        // Since mips weren't regenerated, the last render task shouldn't have changed.
        REPORTER_ASSERT(reporter,
                    mipmapRTCTask.get() == drawingManager->getLastRenderTask(mipmapProxy.get()));

        // Draw the stil-dirty mipmap texture into a second target with mipmap filtering.
        rtc2 = draw_mipmap_into_new_render_target(dContext.get(), colorType, alphaType,
                                                  std::move(mipmapView), MipmapMode::kLinear);
        rtc2Task = sk_ref_sp(rtc2->testingOnly_PeekLastOpsTask());

        // Make sure the mipmap texture now has a new last render task that regenerates the mips,
        // and that the mipmaps are now clean.
        auto mipRegenTask2 = drawingManager->getLastRenderTask(mipmapProxy.get());
        REPORTER_ASSERT(reporter, mipRegenTask2);
        REPORTER_ASSERT(reporter, mipmapRTCTask.get() != mipRegenTask2);
        SkASSERT(!mipmapProxy->mipmapsAreDirty());

        // Mip regen tasks don't get added as dependencies until makeClosed().
        dContext->flushAndSubmit();
        REPORTER_ASSERT(reporter, rtc2Task->dependsOn(mipRegenTask2));
    }
}
