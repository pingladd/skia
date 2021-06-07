/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/GrProgramDesc.h"

#include "include/private/SkChecksum.h"
#include "include/private/SkTo.h"
#include "src/gpu/GrGeometryProcessor.h"
#include "src/gpu/GrPipeline.h"
#include "src/gpu/GrProcessor.h"
#include "src/gpu/GrProgramInfo.h"
#include "src/gpu/GrRenderTarget.h"
#include "src/gpu/GrShaderCaps.h"
#include "src/gpu/GrTexture.h"
#include "src/gpu/glsl/GrGLSLFragmentProcessor.h"
#include "src/gpu/glsl/GrGLSLFragmentShaderBuilder.h"

enum {
    kSamplerOrImageTypeKeyBits = 4
};

static inline uint16_t texture_type_key(GrTextureType type) {
    int value = UINT16_MAX;
    switch (type) {
        case GrTextureType::k2D:
            value = 0;
            break;
        case GrTextureType::kExternal:
            value = 1;
            break;
        case GrTextureType::kRectangle:
            value = 2;
            break;
        default:
            SK_ABORT("Unexpected texture type");
            value = 3;
            break;
    }
    SkASSERT((value & ((1 << kSamplerOrImageTypeKeyBits) - 1)) == value);
    return SkToU16(value);
}

static uint32_t sampler_key(GrTextureType textureType, const GrSwizzle& swizzle,
                            const GrCaps& caps) {
    int samplerTypeKey = texture_type_key(textureType);

    static_assert(2 == sizeof(swizzle.asKey()));
    uint16_t swizzleKey = swizzle.asKey();
    return SkToU32(samplerTypeKey | swizzleKey << kSamplerOrImageTypeKeyBits);
}

static void add_geomproc_sampler_keys(GrProcessorKeyBuilder* b,
                                      const GrGeometryProcessor& geomProc,
                                      const GrCaps& caps) {
    int numTextureSamplers = geomProc.numTextureSamplers();
    b->add32(numTextureSamplers, "ppNumSamplers");
    for (int i = 0; i < numTextureSamplers; ++i) {
        const GrGeometryProcessor::TextureSampler& sampler = geomProc.textureSampler(i);
        const GrBackendFormat& backendFormat = sampler.backendFormat();

        uint32_t samplerKey = sampler_key(backendFormat.textureType(), sampler.swizzle(), caps);
        b->add32(samplerKey);

        caps.addExtraSamplerKey(b, sampler.samplerState(), backendFormat);
    }
}

// Currently we allow 8 bits for the class id
static constexpr uint32_t kClassIDBits = 8;

/**
 * Functions which emit processor key info into the key builder.
 * For every effect, we include the effect's class ID (different for every GrProcessor subclass),
 * any information generated by the effect itself (getGLSLProcessorKey), and some meta-information.
 * Shader code may be dependent on properties of the effect not placed in the key by the effect
 * (e.g. pixel format of textures used).
 */
static void gen_geomproc_key(const GrGeometryProcessor& geomProc,
                             const GrCaps& caps,
                             GrProcessorKeyBuilder* b) {
    b->appendComment(geomProc.name());
    b->addBits(kClassIDBits, geomProc.classID(), "geomProcClassID");

    geomProc.getGLSLProcessorKey(*caps.shaderCaps(), b);
    geomProc.getAttributeKey(b);

    add_geomproc_sampler_keys(b, geomProc, caps);
}

static void gen_xp_key(const GrXferProcessor& xp,
                       const GrCaps& caps,
                       const GrPipeline& pipeline,
                       GrProcessorKeyBuilder* b) {
    b->appendComment(xp.name());
    b->addBits(kClassIDBits, xp.classID(), "xpClassID");

    const GrSurfaceOrigin* originIfDstTexture = nullptr;
    GrSurfaceOrigin origin;
    if (pipeline.dstProxyView().proxy()) {
        origin = pipeline.dstProxyView().origin();
        originIfDstTexture = &origin;
    }

    xp.getGLSLProcessorKey(*caps.shaderCaps(), b, originIfDstTexture,
                           pipeline.dstSampleFlags() & GrDstSampleFlags::kAsInputAttachment);
}

static void gen_fp_key(const GrFragmentProcessor& fp,
                       const GrCaps& caps,
                       GrProcessorKeyBuilder* b) {
    b->appendComment(fp.name());
    b->addBits(kClassIDBits, fp.classID(), "fpClassID");
    b->addBits(GrGeometryProcessor::kCoordTransformKeyBits,
               GrGeometryProcessor::ComputeCoordTransformsKey(fp), "fpTransforms");

    if (auto* te = fp.asTextureEffect()) {
        const GrBackendFormat& backendFormat = te->view().proxy()->backendFormat();
        uint32_t samplerKey = sampler_key(backendFormat.textureType(), te->view().swizzle(), caps);
        b->add32(samplerKey, "fpSamplerKey");
        caps.addExtraSamplerKey(b, te->samplerState(), backendFormat);
    }

    fp.getGLSLProcessorKey(*caps.shaderCaps(), b);
    b->add32(fp.numChildProcessors(), "fpNumChildren");

    for (int i = 0; i < fp.numChildProcessors(); ++i) {
        if (auto child = fp.childProcessor(i)) {
            gen_fp_key(*child, caps, b);
        } else {
            // Fold in a sentinel value as the "class ID" for any null children
            b->appendComment("Null");
            b->addBits(kClassIDBits, GrProcessor::ClassID::kNull_ClassID, "fpClassID");
        }
    }
}

static void gen_key(GrProcessorKeyBuilder* b,
                    const GrProgramInfo& programInfo,
                    const GrCaps& caps) {
    gen_geomproc_key(programInfo.geomProc(), caps, b);

    const GrPipeline& pipeline = programInfo.pipeline();
    b->addBits(2, pipeline.numFragmentProcessors(),      "numFPs");
    b->addBits(1, pipeline.numColorFragmentProcessors(), "numColorFPs");
    for (int i = 0; i < pipeline.numFragmentProcessors(); ++i) {
        gen_fp_key(pipeline.getFragmentProcessor(i), caps, b);
    }

    gen_xp_key(pipeline.getXferProcessor(), caps, pipeline, b);

    b->addBits(16, pipeline.writeSwizzle().asKey(), "writeSwizzle");
    // If we knew the shader won't depend on origin, we could skip this (and use the same program
    // for both origins). Instrumenting all fragment processors would be difficult and error prone.
    b->addBits(2, GrGLSLFragmentShaderBuilder::KeyForSurfaceOrigin(programInfo.origin()), "origin");
    b->addBits(1, static_cast<uint32_t>(programInfo.requestedFeatures()), "requestedFeatures");
    b->addBool(pipeline.snapVerticesToPixelCenters(), "snapVertices");
    // The base descriptor only stores whether or not the primitiveType is kPoints. Backend-
    // specific versions (e.g., Vulkan) require more detail
    b->addBool((programInfo.primitiveType() == GrPrimitiveType::kPoints), "isPoints");

    // Put a clean break between the "common" data written by this function, and any backend data
    // appended later. The initial key length will just be this portion (rounded to 4 bytes).
    b->flush();
}

void GrProgramDesc::Build(GrProgramDesc* desc,
                          const GrProgramInfo& programInfo,
                          const GrCaps& caps) {
    desc->reset();
    GrProcessorKeyBuilder b(desc->key());
    gen_key(&b, programInfo, caps);
    desc->fInitialKeyLength = desc->keyLength();
}

SkString GrProgramDesc::Describe(const GrProgramInfo& programInfo,
                                 const GrCaps& caps) {
    GrProgramDesc desc;
    GrProcessorStringKeyBuilder b(desc.key());
    gen_key(&b, programInfo, caps);
    b.flush();
    return b.description();
}
