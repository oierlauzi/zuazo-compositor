#pragma once

#include <zuazo/ZuazoBase.h>
#include <zuazo/Video.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Signal/SourceLayout.h>
#include <zuazo/Math/Transform.h>

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

	explicit Camera(const Math::Transformf& trf = Math::Transformf(),
					Projection projection		= Projection::FRUSTUM,
					float fov					= 45.0f,
					float nearClip				= 1.0f,
					float farClip 				= 1e6f );
	Camera(const Camera& other);
	~Camera();

	Camera& 				operator=(const Camera& other);

	void					setTransform(const Math::Transformf& trf);
	const Math::Transformf&	getTransform() const;

	void					setProjection(Projection proj);
	Projection				getProjection() const;

	void					setFieldOfView(float fov);
	float					getFieldOfView() const;

	void					setNearClip(float near);
	float					getNearClip() const;

	void					setFarClip(float far);
	float					getFarClip() const;

	Math::Mat4x4f			calculateMatrix(const Math::Vec2f& size) const;
	Math::Mat4x4f			calculateProjectionMatrix(const Math::Vec2f& size) const;

private:
	Math::Transformf		m_transform;
	Projection				m_projection;
	float					m_fieldOfView;
	float					m_nearClip;
	float					m_farClip;

};

}
