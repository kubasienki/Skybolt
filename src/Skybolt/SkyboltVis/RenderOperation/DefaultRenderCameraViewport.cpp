/* Copyright 2012-2020 Matthew Reid
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */


#include "DefaultRenderCameraViewport.h"
#include "RenderOperationSequence.h"
#include "RenderOperationUtil.h"
#include "ViewportStateSet.h"

#include "SkyboltVis/Camera.h"
#include "SkyboltVis/GlobalSamplerUnit.h"
#include "SkyboltVis/Scene.h"
#include "SkyboltVis/Renderable/ScreenQuad.h"
#include "SkyboltVis/Renderable/Atmosphere/Bruneton/BruentonAtmosphereRenderOperation.h"
#include "SkyboltVis/Renderable/Clouds/CloudsRenderTexture.h"
#include "SkyboltVis/Renderable/Clouds/CloudsTemporalUpscaling.h"
#include "SkyboltVis/Renderable/Clouds/VolumeCloudsComposite.h"
#include "SkyboltVis/Renderable/Planet/Planet.h"
#include "SkyboltVis/Renderable/Water/WaterMaterialRenderOperation.h"
#include "SkyboltVis/RenderOperation/RenderEnvironmentMap.h"
#include "SkyboltVis/RenderOperation/RenderOperationOrder.h"
#include "SkyboltVis/Shadow/CascadedShadowMapGenerator.h"
#include "SkyboltVis/Shadow/ShadowHelpers.h"
#include "SkyboltVis/Shadow/ShadowMapGenerator.h"
#include "SkyboltVis/Shader/ShaderProgramRegistry.h"

namespace skybolt {
namespace vis {

static std::shared_ptr<ScreenQuad> createFullscreenQuad(const osg::ref_ptr<osg::Program>& program)
{
	osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet();
	stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
	stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
	stateSet->setAttribute(program);
	return std::make_shared<ScreenQuad>(stateSet);
}

static osg::ref_ptr<RenderTexture> createMainPassTexture()
{
	RenderTextureConfig config;
	config.colorTextureFactories = { createScreenTextureFactory(GL_RGBA16F_ARB) };
	config.depthTextureFactory = createScreenTextureFactory(GL_DEPTH_COMPONENT);
	config.multisampleSampleCount = osg::DisplaySettings::instance()->getNumMultiSamples();

	return new RenderTexture(config);
}
//! Only traverses if rendered by the given camera
class CameraOnlyCallback : public osg::NodeCallback
{
public:
	CameraOnlyCallback(osg::ref_ptr<osg::Camera> camera) :
		mCamera(std::move(camera))
	{
		assert(mCamera);
	}

	void operator()(osg::Node* node, osg::NodeVisitor* nv)
	{
		if (nv->asCullVisitor()->getCurrentCamera() == mCamera)
		{
			traverse(node, nv);
		}
	}

private:
	osg::ref_ptr<osg::Camera> mCamera;
};

DefaultRenderCameraViewport::DefaultRenderCameraViewport(const DefaultRenderCameraViewportConfig& config) :
	mScene(config.scene),
	mViewportStateSet(new ViewportStateSet())
{
	setStateSet(mViewportStateSet);

	mMainPassTexture = createMainPassTexture();
	mMainPassTexture->setScene(mScene->getBucketGroup(Scene::Bucket::Default));

	mFinalRenderTarget = createDefaultRenderTarget();
	mFinalRenderTarget->setScene(createFullscreenQuad(config.programs->getRequiredProgram("compositeFinal"))->_getNode());
	mFinalRenderTarget->setRelativeRect(config.relativeRect);

	mSequence = std::make_unique<RenderOperationSequence>();
	addChild(mSequence->getRootNode());

	mSequence->addOperation(createRenderOperationFunction([this] (const RenderContext& context) {
		if (mCamera)
		{
			CameraRenderContext cameraContext(*mCamera);
			(RenderContext&)cameraContext = context;
			cameraContext.lightDirection = -mScene->getPrimaryLightDirection();
			cameraContext.atmosphericDensity = mScene->calcAtmosphericDensity(mCamera->getPosition());
			mScene->updatePreRender(cameraContext);
		}

	}), (int)RenderOperationOrder::PrepareScene);

	mSequence->addOperation(new BruentonAtmosphereRenderOperation(mScene, mViewportStateSet), (int)RenderOperationOrder::PrecomputeAtmosphere);
	mSequence->addOperation(new RenderEnvironmentMap(mScene, mViewportStateSet, *config.programs), (int)RenderOperationOrder::EnvironmentMap);
	mSequence->addOperation(new WaterMaterialRenderOperation(mScene, *config.programs), (int)RenderOperationOrder::WaterMaterial);

	// Shadows
	if (config.shadowParams)
	{
		CascadedShadowMapGeneratorConfig c;
		c.cascadeBoundingDistances = config.shadowParams->cascadeBoundingDistances;
		c.textureSize = config.shadowParams->textureSize;

		mShadowMapGenerator = std::make_unique<CascadedShadowMapGenerator>(c);
		mViewportStateSet->setDefine("ENABLE_SHADOWS");

		osg::ref_ptr<osg::Group> shadowSceneGroup(new osg::Group);
		shadowSceneGroup->getOrCreateStateSet()->setDefine("CAST_SHADOWS");

		osg::ref_ptr<osg::Group> defaultGroup = mScene->getBucketGroup(Scene::Bucket::Default);
		shadowSceneGroup->addChild(defaultGroup);
		for (const auto& generator : mShadowMapGenerator->getGenerators())
		{
			generator->setScene(shadowSceneGroup);
			mSequence->addOperation(generator, (int)RenderOperationOrder::ShadowMap);
		}

		osg::StateSet* ss = mMainPassTexture->getOrCreateStateSet();
		mShadowMapGenerator->configureShadowReceiverStateSet(*ss);
		addShadowMapsToStateSet(mShadowMapGenerator->getTextures(), *ss, int(GlobalSamplerUnit::ShadowCascade0));
	}

	// Clouds
	{
		osg::ref_ptr<RenderTexture> clouds = createCloudsRenderTexture(mScene);
		mSequence->addOperation(clouds, (int)RenderOperationOrder::Clouds);

		osg::ref_ptr<RenderOperation> cloudsTarget = clouds;
		if (config.cloudRenderingParams.enableTemporalUpscaling)
		{
			CloudsTemporalUpscalingConfig upscalingConfig;
			upscalingConfig.colorTextureProvider = [clouds] { return clouds->getOutputTextures().empty() ? nullptr : clouds->getOutputTextures()[0]; };
			upscalingConfig.depthTextureProvider = [clouds] { return clouds->getOutputTextures().empty() ? nullptr : clouds->getOutputTextures()[1]; };
			upscalingConfig.scene = mScene;
			upscalingConfig.upscalingProgram = config.programs->getRequiredProgram("cloudsTemporalUpscaling");
			mCloudsUpscaling = new CloudsTemporalUpscaling(upscalingConfig);
			mSequence->addOperation(mCloudsUpscaling, (int)RenderOperationOrder::Clouds);

			cloudsTarget = mCloudsUpscaling;
		}

		VolumeCloudsCompositeConfig compositeConfig;
		compositeConfig.compositorProgram = config.programs->getRequiredProgram("compositeClouds");
		compositeConfig.colorTexture = cloudsTarget->getOutputTextures().empty() ? nullptr : cloudsTarget->getOutputTextures()[0];
		compositeConfig.depthTexture = cloudsTarget->getOutputTextures().empty() ? nullptr : cloudsTarget->getOutputTextures()[1];

		mCloudsComposite = std::make_shared<VolumeCloudsComposite>(compositeConfig);
		// Because each Viewport may add a mCloudsComposite to the vis::Scene, there will be multiple in the scene,
		// but we should only render each for their own camera.
		// FIXME: This is a hack. Ideally the compositing wouldn't be done in the main scene pass, but we do it here
		// for now so that the clouds can be composited before rendering the main scene transparent objects.
		mCloudsComposite->_getNode()->addCullCallback(new CameraOnlyCallback(mMainPassTexture->getOsgCamera()));

		mSequence->addOperation(createRenderOperationFunction([this, cloudsTarget] (const RenderContext& context) {
			mCloudsComposite->setColorTexture(cloudsTarget->getOutputTextures().empty() ? nullptr : cloudsTarget->getOutputTextures()[0]);
			mCloudsComposite->setDepthTexture(cloudsTarget->getOutputTextures().empty() ? nullptr : cloudsTarget->getOutputTextures()[1]);
		}), (int)RenderOperationOrder::Clouds);
	}

	mSequence->addOperation(mMainPassTexture, (int)RenderOperationOrder::MainPass);

	mSequence->addOperation(createRenderOperationFunction([this] (const RenderContext& context) {
		if (!mMainPassTexture->getOutputTextures().empty())
		{
			mFinalRenderTarget->getOrCreateStateSet()->setTextureAttributeAndModes(0, mMainPassTexture->getOutputTextures().front());
		}
	}), (int)RenderOperationOrder::MainPass);

	mSequence->addOperation(mFinalRenderTarget, (int)RenderOperationOrder::FinalComposite);

	mHudTarget = createDefaultRenderTarget();
	mHudTarget->getOsgCamera()->setClearMask(0);
	mHudTarget->setScene(mScene->getBucketGroup(Scene::Bucket::Hud));
	mSequence->addOperation(mHudTarget, (int)RenderOperationOrder::Hud);
}

DefaultRenderCameraViewport::~DefaultRenderCameraViewport()
{
	mScene->removeObject(mCloudsComposite);
}

void DefaultRenderCameraViewport::setCamera(const CameraPtr& camera)
{
	mCamera = camera;
	if (mCloudsUpscaling)
	{
		mCloudsUpscaling->setCamera(camera);
	}
	mMainPassTexture->setCamera(camera);
	mHudTarget->setCamera(camera);
}

osg::ref_ptr<RenderTarget> DefaultRenderCameraViewport::getFinalRenderTarget() const
{
	return mFinalRenderTarget;
}

static bool getCloudsVisible(const Scene& scene)
{
	if (const auto& planet = scene.getPrimaryPlanet(); planet)
	{
		return planet->getCloudsVisible();
	}
	return false;
}

void DefaultRenderCameraViewport::updatePreRender(const RenderContext& renderContext)
{
	if (!mCamera)
	{
		return;
	}

	mCamera->setAspectRatio(vis::calcAspectRatio(renderContext));
	mViewportStateSet->update(*mCamera);
	mViewportStateSet->merge(*mScene->getStateSet());

	mSequence->updatePreRender(renderContext);

	if (mShadowMapGenerator)
	{
		mShadowMapGenerator->update(*mCamera, -mScene->getPrimaryLightDirection(), mScene->getWrappedNoiseOrigin());
	}

	bool cloudsVisible = getCloudsVisible(*mScene);
	if (cloudsVisible != mCloudsCompositeInScene)
	{
		mCloudsCompositeInScene = cloudsVisible;
		if (cloudsVisible)
		{
			mScene->addObject(mCloudsComposite);
		}
		else
		{
			mScene->removeObject(mCloudsComposite);
		}
	}
}

std::vector<osg::ref_ptr<osg::Texture>> DefaultRenderCameraViewport::getOutputTextures() const
{
	return mSequence->getOutputTextures();
}
	
} // namespace vis
} // namespace skybolt
