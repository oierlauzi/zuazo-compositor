#include <zuazo/Processors/Compositor.h>

#include <zuazo/Math/Transformations.h>
#include <zuazo/Math/Trigonometry.h>

namespace Zuazo::Processors {

Compositor::Camera::Camera(	const Math::Vec3f& position,
							const Math::Vec3f& target,
							const Math::Vec3f& up,
							Projection projection,
							float fov,
							float nearClip,
							float farClip )
	: m_position(position)
	, m_target(target)
	, m_upDirection(up)
	, m_projection(projection)
	, m_fieldOfView(fov)
	, m_nearClip(nearClip)
	, m_farClip(farClip)
{
}

Compositor::Camera::Camera(const Camera& other) = default;

Compositor::Camera::~Camera() = default;

Compositor::Camera& Compositor::Camera::operator=(const Camera& other) = default;


void Compositor::Camera::setPosition(const Math::Vec3f& position) {
	m_position = position;
}

const Math::Vec3f& Compositor::Camera::getPosition() const {
	return m_position;
}


void Compositor::Camera::setTarget(const Math::Vec3f& target) {
	m_target = target;
}

const Math::Vec3f& Compositor::Camera::getTarget() const {
	return m_target;
}


void Compositor::Camera::setUpDirection(const Math::Vec3f& up) {
	m_upDirection = up;
}

const Math::Vec3f& Compositor::Camera::getUpDirection() const {
	return m_upDirection;
}


void Compositor::Camera::setProjection(Projection proj) {
	m_projection = proj;
}

Compositor::Camera::Projection Compositor::Camera::getProjection() const {
	return m_projection;
}


void Compositor::Camera::setFieldOfView(float fov) {
	m_fieldOfView = fov;
}

float Compositor::Camera::getFieldOfView() const {
	return m_fieldOfView;
}


void Compositor::Camera::setNearClip(float near) {
	m_nearClip = near;
}

float Compositor::Camera::getNearClip() const {
	return m_nearClip;
}


void Compositor::Camera::setFarClip(float far) {
	m_farClip = far;
}

float Compositor::Camera::getFarClip() const {
	return m_farClip;
}


Math::Mat4x4f Compositor::Camera::calculateViewMatrix() const {
	return Math::lookAt(
		getPosition(),		//Eye
		getTarget(),		//Center
		getUpDirection()	//Up
	);
}

Math::Mat4x4f Compositor::Camera::calculateProjectionMatrix(const Math::Vec2f& size) const {
	switch(getProjection()) {
	case Projection::ORTHOGONAL:
		return Math::ortho(
			-size.x / 2.0f, +size.x / 2.0f,	//Left, Right
			-size.y / 2.0f, +size.y / 2.0f,	//Bottom, Top
			getNearClip(), getFarClip()		//Clipping planes
		);

	case Projection::FRUSTUM:
		return Math::perspective(
			Math::deg2rad(getFieldOfView()),//Vertical FOV
			size.x / size.y,				//Aspect ratio
			getNearClip(), getFarClip()		//Clipping planes
		);

	default:
		return Math::Mat4x4f(1.0f);

	}
}

}