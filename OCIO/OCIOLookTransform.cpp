/*
 OCIOLookTransform plugin.
 Convert from one colorspace to another.

 Copyright (C) 2014 INRIA
 Author: Frederic Devernay <frederic.devernay@inria.fr>

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 INRIA
 Domaine de Voluceau
 Rocquencourt - B.P. 105
 78153 Le Chesnay Cedex - France

 */


#include "OCIOLookTransform.h"

//#include <iostream>
#include <memory>

#include <GenericOCIO.h>

#include <ofxsProcessing.H>
#include <ofxsCopier.h>
#include <ofxsMacros.h>
#include <ofxNatron.h>

#include "IOUtility.h"

#ifdef OFX_IO_USING_OCIO

#define kPluginName "OCIOLookTransformOFX"
#define kPluginGrouping "Color/OCIO"
#define kPluginDescription \
"OpenColorIO LookTransform\n\n" \
"A 'look' is a named color transform, intended to modify the look of an " \
"image in a 'creative' manner (as opposed to a colorspace definion which " \
"tends to be technically/mathematically defined).\n\n" \
"Examples of looks may be a neutral grade, to be applied to film scans " \
"prior to VFX work, or a per-shot DI grade decided on by the director, " \
"to be applied just before the viewing transform.\n\n" \
"OCIOLooks must be predefined in the OpenColorIO configuration before usage, " \
"and often reference per-shot/sequence LUTs/CCs.\n\n" \
"See the \'Look Combination\' parameter for further syntax details.\n\n" \
"See opencolorio.org for look configuration customization examples."

#define kPluginIdentifier "fr.inria.openfx.OCIOLookTransform"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kParamLookChoice "lookChoice"
#define kParamLookChoiceLabel "Look"
#define kParamLookChoiceHint "Look to apply (if \"Single Look\" is checked) or append to the Look Combination (when the \"Append\" button is pressed)."

#define kParamLookAppend "append"
#define kParamLookAppendLabel "Append Look to Combination"
#define kParamLookAppendHint "Append the selected Look to the Look Combination"

#define kParamSingleLook "singleLook"
#define kParamSingleLookLabel "Single Look"
#define kParamSingleLookHint "When checked, only the selected Look is applied. When not checked, the Look Combination is applied."

#define kParamLookCombination "lookCombination"
#define kParamLookCombinationLabel "Look Combination"
#define kParamLookCombinationHint \
"Specify the look(s) to apply.\n" \
"This may be empty, the name of a single look, or a combination of looks using the 'look syntax'.\n" \
"If it is empty, no look is applied.\n" \
"Look Syntax:\n" \
"Multiple looks are combined with commas: 'firstlook, secondlook'\n" \
"Direction is specified with +/- prefixes: '+firstlook, -secondlook'\n" \
"Missing look 'fallbacks' specified with |: 'firstlook, -secondlook | -secondlook'"

#define kParamDirection "direction"
#define kParamDirectionLabel "Direction"
#define kParamDirectionHint "Transform direction."
#define kParamDirectionOptionForward "Forward"
#define kParamDirectionOptionInverse "Inverse"

namespace OCIO = OCIO_NAMESPACE;

static bool gHostIsNatron   = false;

// ChoiceParamType may be OFX::ChoiceParamDescriptor or OFX::ChoiceParam
template <typename ChoiceParamType>
static void
buildLookChoiceMenu(OCIO::ConstConfigRcPtr config,
                ChoiceParamType* choice)
{
    choice->resetOptions();
    if (!config) {
        return;
    }
    for (int i = 0; i < config->getNumLooks(); ++i) {
        choice->appendOption(config->getLookNameByIndex(i));
    }
}

class OCIOLookTransformPlugin : public OFX::ImageEffect
{
public:

    OCIOLookTransformPlugin(OfxImageEffectHandle handle);

    virtual ~OCIOLookTransformPlugin();

    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    /* override is identity */
    virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &identityTime) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;

    /* override changed clip */
    virtual void changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName) OVERRIDE FINAL;

    // override the rod call
    //virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

    // override the roi call
    //virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois) OVERRIDE FINAL;

private:
    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI &renderWindow,
                       const OFX::Image* srcImg,
                       OFX::Image* dstImg)
    {
        const void* srcPixelData;
        OfxRectI srcBounds;
        OFX::PixelComponentEnum srcPixelComponents;
        OFX::BitDepthEnum srcBitDepth;
        int srcRowBytes;
        getImageData(srcImg, &srcPixelData, &srcBounds, &srcPixelComponents, &srcBitDepth, &srcRowBytes);
        void* dstPixelData;
        OfxRectI dstBounds;
        OFX::PixelComponentEnum dstPixelComponents;
        OFX::BitDepthEnum dstBitDepth;
        int dstRowBytes;
        getImageData(dstImg, &dstPixelData, &dstBounds, &dstPixelComponents, &dstBitDepth, &dstRowBytes);
        copyPixelData(unpremult,
                      premult,
                      maskmix,
                      time,
                      renderWindow,
                      srcPixelData, srcBounds, srcPixelComponents, srcBitDepth, srcRowBytes,
                      dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
    }

    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI &renderWindow,
                       const void *srcPixelData,
                       const OfxRectI& srcBounds,
                       OFX::PixelComponentEnum srcPixelComponents,
                       OFX::BitDepthEnum srcBitDepth,
                       int srcRowBytes,
                       OFX::Image* dstImg)
    {
        void* dstPixelData;
        OfxRectI dstBounds;
        OFX::PixelComponentEnum dstPixelComponents;
        OFX::BitDepthEnum dstBitDepth;
        int dstRowBytes;
        getImageData(dstImg, &dstPixelData, &dstBounds, &dstPixelComponents, &dstBitDepth, &dstRowBytes);
        copyPixelData(unpremult,
                      premult,
                      maskmix,
                      time,
                      renderWindow,
                      srcPixelData, srcBounds, srcPixelComponents, srcBitDepth, srcRowBytes,
                      dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
    }

    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI &renderWindow,
                       const OFX::Image* srcImg,
                       void *dstPixelData,
                       const OfxRectI& dstBounds,
                       OFX::PixelComponentEnum dstPixelComponents,
                       OFX::BitDepthEnum dstBitDepth,
                       int dstRowBytes)
    {
        const void* srcPixelData;
        OfxRectI srcBounds;
        OFX::PixelComponentEnum srcPixelComponents;
        OFX::BitDepthEnum srcBitDepth;
        int srcRowBytes;
        getImageData(srcImg, &srcPixelData, &srcBounds, &srcPixelComponents, &srcBitDepth, &srcRowBytes);
        copyPixelData(unpremult,
                      premult,
                      maskmix,
                      time,
                      renderWindow,
                      srcPixelData, srcBounds, srcPixelComponents, srcBitDepth, srcRowBytes,
                      dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
    }

    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI &renderWindow,
                       const void *srcPixelData,
                       const OfxRectI& srcBounds,
                       OFX::PixelComponentEnum srcPixelComponents,
                       OFX::BitDepthEnum srcPixelDepth,
                       int srcRowBytes,
                       void *dstPixelData,
                       const OfxRectI& dstBounds,
                       OFX::PixelComponentEnum dstPixelComponents,
                       OFX::BitDepthEnum dstBitDepth,
                       int dstRowBytes);

    void setupAndCopy(OFX::PixelProcessorFilterBase & processor,
                      double time,
                      const OfxRectI &renderWindow,
                      const void *srcPixelData,
                      const OfxRectI& srcBounds,
                      OFX::PixelComponentEnum srcPixelComponents,
                      OFX::BitDepthEnum srcPixelDepth,
                      int srcRowBytes,
                      void *dstPixelData,
                      const OfxRectI& dstBounds,
                      OFX::PixelComponentEnum dstPixelComponents,
                      OFX::BitDepthEnum dstPixelDepth,
                      int dstRowBytes);

    void apply(double time, const OfxRectI& renderWindow, float *pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int rowBytes);

    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *dstClip_;
    OFX::Clip *srcClip_;
    OFX::Clip *maskClip_;

    OFX::ChoiceParam* _lookChoice;
    OFX::PushButtonParam* _lookAppend;
    OFX::BooleanParam* _singleLook;
    OFX::StringParam* _lookCombination;

    OFX::ChoiceParam *direction_;

    OFX::BooleanParam* _premult;
    OFX::ChoiceParam* _premultChannel;
    OFX::DoubleParam* _mix;
    OFX::BooleanParam* _maskInvert;

    std::auto_ptr<GenericOCIO> _ocio;
};

OCIOLookTransformPlugin::OCIOLookTransformPlugin(OfxImageEffectHandle handle)
: OFX::ImageEffect(handle)
, dstClip_(0)
, srcClip_(0)
, maskClip_(0)
, _ocio(new GenericOCIO(this))
{
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    assert(dstClip_ && (dstClip_->getPixelComponents() == OFX::ePixelComponentRGBA || dstClip_->getPixelComponents() == OFX::ePixelComponentRGB));
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert(srcClip_ && (srcClip_->getPixelComponents() == OFX::ePixelComponentRGBA || srcClip_->getPixelComponents() == OFX::ePixelComponentRGB));
    maskClip_ = getContext() == OFX::eContextFilter ? NULL : fetchClip(getContext() == OFX::eContextPaint ? "Brush" : "Mask");
    assert(!maskClip_ || maskClip_->getPixelComponents() == OFX::ePixelComponentAlpha);
    _lookChoice = fetchChoiceParam(kParamLookChoice);
    _lookAppend = fetchPushButtonParam(kParamLookAppend);
    _singleLook = fetchBooleanParam(kParamSingleLook);
    _lookCombination = fetchStringParam(kParamLookCombination);
    assert(_lookChoice && _lookAppend && _singleLook && _lookCombination);
    direction_ = fetchChoiceParam(kParamDirection);
    assert(direction_);
    _premult = fetchBooleanParam(kParamPremult);
    _premultChannel = fetchChoiceParam(kParamPremultChannel);
    assert(_premult && _premultChannel);
    _mix = fetchDoubleParam(kParamMix);
    _maskInvert = fetchBooleanParam(kParamMaskInvert);
    assert(_mix && _maskInvert);

    bool singleLook;
    _singleLook->getValue(singleLook);
    _lookChoice->setEvaluateOnChange(singleLook);
    _lookCombination->setEnabled(!singleLook);
    _lookCombination->setEvaluateOnChange(!singleLook);
}

OCIOLookTransformPlugin::~OCIOLookTransformPlugin()
{
}

/* set up and run a copy processor */
void
OCIOLookTransformPlugin::setupAndCopy(OFX::PixelProcessorFilterBase & processor,
                                   double time,
                                   const OfxRectI &renderWindow,
                                   const void *srcPixelData,
                                   const OfxRectI& srcBounds,
                                   OFX::PixelComponentEnum srcPixelComponents,
                                   OFX::BitDepthEnum srcPixelDepth,
                                   int srcRowBytes,
                                   void *dstPixelData,
                                   const OfxRectI& dstBounds,
                                   OFX::PixelComponentEnum dstPixelComponents,
                                   OFX::BitDepthEnum dstPixelDepth,
                                   int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // make sure bit depths are sane
    if(srcPixelDepth != dstPixelDepth || srcPixelComponents != dstPixelComponents) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    std::auto_ptr<OFX::Image> mask(getContext() != OFX::eContextFilter ? maskClip_->fetchImage(time) : 0);
    std::auto_ptr<OFX::Image> orig(srcClip_->fetchImage(time));
    if (getContext() != OFX::eContextFilter && maskClip_->isConnected()) {
        bool maskInvert;
        _maskInvert->getValueAtTime(time, maskInvert);
        processor.doMasking(true);
        processor.setMaskImg(mask.get(), maskInvert);
    }

    if (!orig.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    // set the images
    assert(orig.get() && dstPixelData && srcPixelData);
    processor.setOrigImg(orig.get());
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    processor.setSrcImg(srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, 0);

    // set the render window
    processor.setRenderWindow(renderWindow);

    bool premult;
    int premultChannel;
    _premult->getValueAtTime(time, premult);
    _premultChannel->getValueAtTime(time, premultChannel);
    double mix;
    _mix->getValueAtTime(time, mix);
    processor.setPremultMaskMix(premult, premultChannel, mix);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

void
OCIOLookTransformPlugin::copyPixelData(bool unpremult,
                                    bool premult,
                                    bool maskmix,
                                    double time,
                                    const OfxRectI& renderWindow,
                                    const void *srcPixelData,
                                    const OfxRectI& srcBounds,
                                    OFX::PixelComponentEnum srcPixelComponents,
                                    OFX::BitDepthEnum srcPixelDepth,
                                    int srcRowBytes,
                                    void *dstPixelData,
                                    const OfxRectI& dstBounds,
                                    OFX::PixelComponentEnum dstPixelComponents,
                                    OFX::BitDepthEnum dstBitDepth,
                                    int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);
    // do the rendering
    if (dstBitDepth != OFX::eBitDepthFloat || (dstPixelComponents != OFX::ePixelComponentRGBA && dstPixelComponents != OFX::ePixelComponentRGB && dstPixelComponents != OFX::ePixelComponentAlpha)) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    if (!unpremult && !premult && !maskmix) {
        if (dstPixelComponents == OFX::ePixelComponentRGBA) {
            OFX::PixelCopier<float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == OFX::ePixelComponentRGB) {
            OFX::PixelCopier<float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        }  else if (dstPixelComponents == OFX::ePixelComponentAlpha) {
            OFX::PixelCopier<float, 1, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        }
    } else if (unpremult && !premult && !maskmix) {
        if (dstPixelComponents == OFX::ePixelComponentRGBA) {
            OFX::PixelCopierUnPremult<float, 4, 1, float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == OFX::ePixelComponentRGB) {
            OFX::PixelCopierUnPremult<float, 3, 1, float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        }  else if (dstPixelComponents == OFX::ePixelComponentAlpha) {
            OFX::PixelCopierUnPremult<float, 1, 1, float, 1, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } // switch

    } else if (!unpremult && !premult && maskmix) {
        if (dstPixelComponents == OFX::ePixelComponentRGBA) {
            OFX::PixelCopierMaskMix<float, 4, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == OFX::ePixelComponentRGB) {
            OFX::PixelCopierMaskMix<float, 3, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        }  else if (dstPixelComponents == OFX::ePixelComponentAlpha) {
            OFX::PixelCopierMaskMix<float, 1, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } // switch

    } else if (!unpremult && premult && maskmix) {
        if (dstPixelComponents == OFX::ePixelComponentRGBA) {
            OFX::PixelCopierPremultMaskMix<float, 4, 1, float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == OFX::ePixelComponentRGB) {
            OFX::PixelCopierPremultMaskMix<float, 3, 1, float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        }  else if (dstPixelComponents == OFX::ePixelComponentAlpha) {
            OFX::PixelCopierPremultMaskMix<float, 1, 1, float, 1, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
        } // switch

    } else {
        assert(false); // should never happen
    }
}

void
OCIOLookTransformPlugin::apply(double time, const OfxRectI& renderWindow, float *pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int rowBytes)
{
    // are we in the image bounds
    if(renderWindow.x1 < bounds.x1 || renderWindow.x1 >= bounds.x2 || renderWindow.y1 < bounds.y1 || renderWindow.y1 >= bounds.y2 ||
       renderWindow.x2 <= bounds.x1 || renderWindow.x2 > bounds.x2 || renderWindow.y2 <= bounds.y1 || renderWindow.y2 > bounds.y2) {
        throw std::runtime_error("OCIO: render window outside of image bounds");
    }
    if (pixelComponents != OFX::ePixelComponentRGBA && pixelComponents != OFX::ePixelComponentRGB) {
        throw std::runtime_error("OCIO: invalid components (only RGB and RGBA are supported)");
    }

    OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
    assert(config);

    bool singleLook;
    _singleLook->getValueAtTime(time, singleLook);
    std::string lookCombination;
    _lookCombination->getValueAtTime(time, lookCombination);
    if (_ocio->isIdentity(time) && !singleLook && lookCombination.empty()) {
        return; // isIdentity
    }
    std::string inputSpace;
    _ocio->getInputColorspaceAtTime(time, inputSpace);
    std::string look;
    if (singleLook) {
        int lookChoice_i;
        _lookChoice->getValueAtTime(time, lookChoice_i);
        look = config->getLookNameByIndex(lookChoice_i);
    } else {
        look = lookCombination;
    }
    int direction_i;
    direction_->getValueAtTime(time, direction_i);
    std::string outputSpace;
    _ocio->getOutputColorspaceAtTime(time, outputSpace);

    OCIOProcessor processor(*this);
    // set the images
    processor.setDstImg(pixelData, bounds, pixelComponents, OFX::eBitDepthFloat, rowBytes);

    try {
        OCIO::TransformDirection direction = OCIO::TRANSFORM_DIR_UNKNOWN;
        OCIO::LookTransformRcPtr transform = OCIO::LookTransform::Create();
        transform->setLooks(look.c_str());

        if (direction_i == 0) {
            transform->setSrc(inputSpace.c_str());
            transform->setDst(outputSpace.c_str());
            direction = OCIO::TRANSFORM_DIR_FORWARD;
        } else {
            // The TRANSFORM_DIR_INVERSE applies an inverse for the end-to-end transform,
            // which would otherwise do dst->inv look -> src.
            // This is an unintuitive result for the artist (who would expect in, out to
            // remain unchanged), so we account for that here by flipping src/dst

            transform->setSrc(outputSpace.c_str());
            transform->setDst(inputSpace.c_str());
            direction = OCIO::TRANSFORM_DIR_INVERSE;
        }

        processor.setValues(config, transform, direction);
    } catch (const OCIO::Exception &e) {
        setPersistentMessage(OFX::Message::eMessageError, "", e.what());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    // set the render window
    processor.setRenderWindow(renderWindow);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

/* Override the render */
void
OCIOLookTransformPlugin::render(const OFX::RenderArguments &args)
{
    if (!srcClip_) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    assert(srcClip_);
    std::auto_ptr<const OFX::Image> srcImg(srcClip_->fetchImage(args.time));
    if (!srcImg.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (srcImg->getRenderScale().x != args.renderScale.x ||
        srcImg->getRenderScale().y != args.renderScale.y ||
        srcImg->getField() != args.fieldToRender) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    OFX::BitDepthEnum srcBitDepth = srcImg->getPixelDepth();
    OFX::PixelComponentEnum srcComponents = srcImg->getPixelComponents();

    if (!dstClip_) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    assert(dstClip_);
    std::auto_ptr<OFX::Image> dstImg(dstClip_->fetchImage(args.time));
    if (!dstImg.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (dstImg->getRenderScale().x != args.renderScale.x ||
        dstImg->getRenderScale().y != args.renderScale.y ||
        dstImg->getField() != args.fieldToRender) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    OFX::BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if (dstBitDepth != OFX::eBitDepthFloat || dstBitDepth != srcBitDepth) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    OFX::PixelComponentEnum dstComponents  = dstImg->getPixelComponents();
    if ((dstComponents != OFX::ePixelComponentRGBA && dstComponents != OFX::ePixelComponentRGB && dstComponents != OFX::ePixelComponentAlpha) ||
        dstComponents != srcComponents) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    // are we in the image bounds
    OfxRectI dstBounds = dstImg->getBounds();
    if(args.renderWindow.x1 < dstBounds.x1 || args.renderWindow.x1 >= dstBounds.x2 || args.renderWindow.y1 < dstBounds.y1 || args.renderWindow.y1 >= dstBounds.y2 ||
       args.renderWindow.x2 <= dstBounds.x1 || args.renderWindow.x2 > dstBounds.x2 || args.renderWindow.y2 <= dstBounds.y1 || args.renderWindow.y2 > dstBounds.y2) {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
        //throw std::runtime_error("render window outside of image bounds");
    }

    const void* srcPixelData = NULL;
    OfxRectI bounds;
    OFX::PixelComponentEnum pixelComponents;
    OFX::BitDepthEnum bitDepth;
    int srcRowBytes;
    getImageData(srcImg.get(), &srcPixelData, &bounds, &pixelComponents, &bitDepth, &srcRowBytes);

    // allocate temporary image
    int pixelBytes = getPixelBytes(srcComponents, srcBitDepth);
    int tmpRowBytes = (args.renderWindow.x2-args.renderWindow.x1) * pixelBytes;
    size_t memSize = (args.renderWindow.y2-args.renderWindow.y1) * tmpRowBytes;
    OFX::ImageMemory mem(memSize,this);
    float *tmpPixelData = (float*)mem.lock();

    bool premult;
    _premult->getValueAtTime(args.time, premult);

    // copy renderWindow to the temporary image
    copyPixelData(premult, false, false, args.time, args.renderWindow, srcPixelData, bounds, pixelComponents, bitDepth, srcRowBytes, tmpPixelData, args.renderWindow, pixelComponents, bitDepth, tmpRowBytes);

    ///do the color-space conversion
    apply(args.time, args.renderWindow, tmpPixelData, args.renderWindow, pixelComponents, tmpRowBytes);

    // copy the color-converted window and apply masking
    copyPixelData(false, premult, true, args.time, args.renderWindow, tmpPixelData, args.renderWindow, pixelComponents, bitDepth, tmpRowBytes, dstImg.get());
}

bool
OCIOLookTransformPlugin::isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &/*identityTime*/)
{
    if (_ocio->isIdentity(args.time)) {
        bool singleLook;
        _singleLook->getValueAtTime(args.time, singleLook);
        if (!singleLook) {
            std::string lookCombination;
            _lookCombination->getValueAtTime(args.time, lookCombination);
            if (lookCombination.empty()) {
                identityClip = srcClip_;
                return true;
            }
        }
    }
    return false;
}

void
OCIOLookTransformPlugin::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName)
{
    if (paramName == kParamLookAppend) {
        OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
        std::string lookCombination;
        int lookChoice;
        _lookCombination->getValueAtTime(args.time, lookCombination);
        _lookChoice->getValueAtTime(args.time, lookChoice);
        std::string look = config->getLookNameByIndex(lookChoice);
        if (!look.empty()) {
            if (lookCombination.empty()) {
                lookCombination = look;
            } else {
                lookCombination += ", ";
                lookCombination += look;
            }
            _lookCombination->setValue(lookCombination);
        }
    } else if (paramName == kParamSingleLook && args.reason == OFX::eChangeUserEdit) {
        bool singleLook;
        _singleLook->getValueAtTime(args.time, singleLook);
        _lookChoice->setEvaluateOnChange(singleLook);
        _lookCombination->setEnabled(!singleLook);
        _lookCombination->setEvaluateOnChange(!singleLook);
    } else {
        _ocio->changedParam(args, paramName);
    }
    // this must be done after handling by GenericOCIO
    if (paramName == kOCIOParamConfigFileName && args.reason == OFX::eChangeUserEdit) {
        if (gHostIsNatron) {
            // the choice menu can only be modified in Natron
            // Natron supports changing the entries in a choiceparam
            // Nuke (at least up to 8.0v3) does not
            OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
            buildLookChoiceMenu(config, _lookChoice);
        } else {
            _lookChoice->setEnabled(false);
            _lookChoice->setIsSecret(true);
            _lookAppend->setEnabled(false);
            _lookAppend->setIsSecret(true);
            _singleLook->setValue(true);
            _singleLook->setIsSecret(true);
            _singleLook->setEnabled(false);
        }
    }
}

void
OCIOLookTransformPlugin::changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName)
{
    if (clipName == kOfxImageEffectSimpleSourceClipName && srcClip_ && args.reason == OFX::eChangeUserEdit) {
        switch (srcClip_->getPreMultiplication()) {
            case OFX::eImageOpaque:
                break;
            case OFX::eImagePreMultiplied:
                _premult->setValue(true);
                break;
            case OFX::eImageUnPreMultiplied:
                _premult->setValue(false);
                break;
        }
    }
}

using namespace OFX;

mDeclarePluginFactory(OCIOLookTransformPluginFactory, {}, {});

/** @brief The basic describe function, passed a plugin descriptor */
void OCIOLookTransformPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextPaint);

    // add supported pixel depths
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void OCIOLookTransformPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum context)
{
    // Source clip only in the filter context
    // create the mandated source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->setSupportsTiles(kSupportsTiles);

    if (context == eContextGeneral || context == eContextPaint) {
        ClipDescriptor *maskClip = context == eContextGeneral ? desc.defineClip("Mask") : desc.defineClip("Brush");
        maskClip->addSupportedComponent(ePixelComponentAlpha);
        maskClip->setTemporalClipAccess(false);
        if (context == eContextGeneral) {
            maskClip->setOptional(true);
        }
        maskClip->setSupportsTiles(kSupportsTiles);
        maskClip->setIsMask(true);
    }

    gHostIsNatron = (OFX::getImageEffectHostDescription()->hostName == kOfxNatronHostName);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");
    // insert OCIO parameters
    GenericOCIO::describeInContextInput(desc, context, page, OCIO::ROLE_REFERENCE);
    OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kParamSingleLook);
        param->setLabels(kParamSingleLookLabel, kParamSingleLookLabel, kParamSingleLookLabel);
        param->setHint(kParamSingleLookHint);
        if (config) {
            param->setDefault(true);
        } else {
            param->setDefault(false);
            param->setEnabled(false);
            param->setIsSecret(true);
        }
        page->addChild(*param);
    }
    {
        OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamLookChoice);
        param->setLabels(kParamLookChoiceLabel, kParamLookChoiceLabel, kParamLookChoiceLabel);
        param->setHint(kParamLookChoiceHint);
        if (config) {
            buildLookChoiceMenu(config, param);
        } else {
            param->setEnabled(false);
            param->setIsSecret(true);
        }
        param->setAnimates(true);
        page->addChild(*param);
    }
    {
        OFX::PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamLookAppend);
        param->setLabels(kParamLookAppendLabel, kParamLookAppendLabel, kParamLookAppendLabel);
        param->setHint(kParamLookAppendHint);
        if (!config) {
            param->setEnabled(false);
            param->setIsSecret(true);
        }
        page->addChild(*param);
    }
    {
        OFX::StringParamDescriptor* param = desc.defineStringParam(kParamLookCombination);
        param->setLabels(kParamLookCombinationLabel, kParamLookCombinationLabel, kParamLookCombinationLabel);
        param->setHint(kParamLookCombinationHint);
        page->addChild(*param);
    }
    {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamDirection);
        param->setLabels(kParamDirectionLabel, kParamDirectionLabel, kParamDirectionLabel);
        param->setHint(kParamDirectionHint);
        param->appendOption(kParamDirectionOptionForward);
        param->appendOption(kParamDirectionOptionInverse);
        param->setDefault(0);
        page->addChild(*param);
    }
    GenericOCIO::describeInContextOutput(desc, context, page, OCIO::ROLE_REFERENCE);

    ofxsPremultDescribeParams(desc, page);
    ofxsMaskMixDescribeParams(desc, page);
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect* OCIOLookTransformPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new OCIOLookTransformPlugin(handle);
}


void getOCIOLookTransformPluginID(OFX::PluginFactoryArray &ids)
{
    static OCIOLookTransformPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}


#else // !OFX_IO_USING_OCIO

void getOCIOLookTransformPluginID(OFX::PluginFactoryArray &ids)
{
}

#endif

