#pragma once

#include <zuazo/DepthStencilFormat.h>
#include <zuazo/Graphics/Vulkan.h>
#include <zuazo/Math/Transform.h>
#include <zuazo/Utils/Pimpl.h>
#include <zuazo/Utils/Limit.h>

#include <functional>

namespace Zuazo {

class RendererBase {
public:
	class Camera;

	using DepthStencilFormatCallback = std::function<void(RendererBase&, const Utils::Limit<DepthStencilFormat>&)>;
	using CameraCallback = std::function<void(RendererBase&, const Camera& camera)>;
	using RenderPassQueryCallback = std::function<vk::RenderPass(const RendererBase&)>;

	enum DescriptorBindings {
		DESCRIPTOR_BINDING_PROJECTION_MATRIX,
		DESCRIPTOR_BINDING_COLOR_TRANSFER,

		DESCRIPTOR_COUNT
	};


	RendererBase(	Utils::Limit<DepthStencilFormat> depthStencil = {},
					DepthStencilFormatCallback internalDepthStencilCbk = {},
					CameraCallback cameraCbk = {},
					RenderPassQueryCallback renderPassQueryCbk = {} );
	RendererBase(const RendererBase& other) = delete;
	RendererBase(RendererBase&& other);
	virtual ~RendererBase();

	RendererBase& 							operator=(const RendererBase& other) = delete;
	RendererBase&							operator=(RendererBase&& other);

	void									setDepthStencilFormatCompatibilityCallback(DepthStencilFormatCallback cbk);
	const DepthStencilFormatCallback&		getDepthStencilFormatCompatibilityCallback() const;

	void									setDepthStencilFormatCallback(DepthStencilFormatCallback cbk);
	const DepthStencilFormatCallback&		getDepthStencilFormatCallback() const;

	void									setDepthStencilFormatLimits(Utils::Limit<DepthStencilFormat> lim);
	const Utils::Limit<DepthStencilFormat>&	getDepthStencilFormatLimits() const;
	const Utils::Limit<DepthStencilFormat>& getDepthStencilFormatCompatibility() const;
	const Utils::Limit<DepthStencilFormat>&	getDepthStencilFormat() const;

	void									setCamera(const Camera& cam);
	const Camera&							getCamera() const;

	vk::RenderPass							getRenderPass() const;

	static vk::DescriptorSetLayout			getDescriptorSetLayout(const Graphics::Vulkan& vulkan);

protected:
	void									setDepthStencilFormatCompatibility(Utils::Limit<DepthStencilFormat> comp);

	void									setInternalDepthStencilFormatCallback(DepthStencilFormatCallback cbk);
	const DepthStencilFormatCallback&		getInternalDepthStencilFormatCallback() const;

	void									setCameraCallback(CameraCallback cbk);
	const CameraCallback&					getCameraCallback() const;

	void									setRenderPassQueryCallbackCallback(RenderPassQueryCallback cbk);
	const RenderPassQueryCallback&			getRenderPassQueryCallback() const;

private:
	struct Impl;
	Utils::Pimpl<Impl>		m_impl;		

};



class RendererBase::Camera {
public:
	enum class Projection {
		ORTHOGONAL,
		FRUSTUM
	};

	explicit Camera(const Math::Transformf& trf = Math::Transformf(),
					Projection projection		= Projection::ORTHOGONAL,
					float nearClip				= -10e3,
					float farClip 				= +10e3,
					float fov					= 0.0f ); // FOV Unused for orthogonal
	Camera(const Camera& other);
	~Camera();

	Camera& 				operator=(const Camera& other);

	bool					operator==(const Camera& other) const;
	bool					operator!=(const Camera& other) const;

	void					setTransform(const Math::Transformf& trf);
	const Math::Transformf&	getTransform() const;

	void					setProjection(Projection proj);
	Projection				getProjection() const;

	void					setNearClip(float near);
	float					getNearClip() const;

	void					setFarClip(float far);
	float					getFarClip() const;

	void					setFieldOfView(float fov);
	float					getFieldOfView() const;

	Math::Mat4x4f			calculateMatrix(const Math::Vec2f& size) const;
	Math::Mat4x4f			calculateViewMatrix() const;
	Math::Mat4x4f			calculateProjectionMatrix(const Math::Vec2f& size) const;

private:
	Math::Transformf		m_transform;
	Projection				m_projection;
	float					m_nearClip;
	float					m_farClip;
	float					m_fieldOfView;

};

}