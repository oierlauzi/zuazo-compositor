#include <zuazo/Processors/Compositor.h>

namespace Zuazo::Processors {

Compositor::Camera::Camera(	const Math::Vec3f& position,
							const Math::Vec3f& target,
							const Math::Vec3f& up )
	: m_position(position)
	, m_target(target)
	, m_upDirection(up)
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

}