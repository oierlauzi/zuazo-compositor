#pragma once

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Signal/SourceLayout.h>

namespace Zuazo::Processors {

struct CompositorImpl;
class Compositor 
	: private Utils::Pimpl<CompositorImpl>
	, public ZuazoBase
	, public VideoBase
	, public Signal::SourceLayout<Video>
{
	friend CompositorImpl;
public:
	enum class RenderingStage {
		BACKGROUND,
		SCENE,
		FOREGROUND
	};

	class Camera;

	struct FrameBufferFormat {
		ColorFormat				colorFormat;
		//DepthStencilFormat	depthStencilFormat
	};

	Compositor(	Instance& instance, 
				std::string name, 
				VideoMode videoMode = VideoMode::ANY );
	Compositor(const Compositor& other) = delete;
	Compositor(Compositor&& other);
	virtual ~Compositor();

	Compositor& 			operator=(const Compositor& other) = delete;
	Compositor& 			operator=(Compositor&& other);

	void					setLayerCount(size_t count);
	size_t					getLayerCount() const;

	void					setCamera(const Camera& cam);
	const Camera&			getCamera() const;

	static vk::RenderPass	createRenderPass(	const Graphics::Vulkan& vulkan, 
												const FrameBufferFormat& fbFormat );

};



class Compositor::Camera {
public:
	enum class Projection {
		ORTHOGONAL,
		FRUSTUM
	};

	Camera(	const Math::Vec3f& position	= Math::Vec3f(0.0f, 0.0f, 0.0f),
			const Math::Vec3f& target 	= Math::Vec3f(1.0f, 0.0f, 0.0f),
			const Math::Vec3f& up 		= Math::Vec3f(0.0f, 1.0f, 0.0f) );
	Camera(const Camera& other);
	~Camera();

	Camera& 				operator=(const Camera& other);

	void					setPosition(const Math::Vec3f& position);
	const Math::Vec3f&		getPosition() const;
	
	void					setTarget(const Math::Vec3f& target);
	const Math::Vec3f&		getTarget() const;

	void					setUpDirection(const Math::Vec3f& up);
	const Math::Vec3f&		getUpDirection() const;

	void					setProjection(Projection proj);
	Projection				getProjection() const;

	void					setFieldOfView(float fov);
	float					getFieldOfView() const;

	void					setNearClip(float near);
	float					getNearClip() const;

	void					setFarClip(float far);
	float					getFarClip() const;

private:
	Math::Vec3f				m_position;
	Math::Vec3f				m_target;
	Math::Vec3f				m_upDirection;

	Projection				m_projection;
	float					m_fieldOfView;
	float					m_nearClip;
	float					m_farClip;

};

}
