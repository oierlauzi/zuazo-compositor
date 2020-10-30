#include <zuazo/Processors/Layers/LayerBase.h>

#include <zuazo/Math/Transformations.h>

namespace Zuazo::Processors::Layers {


LayerBase::Transform ::Transform(	Math::Vec3f position,
									Math::Quaternionf rotation,
									Math::Vec3f scale,
									Math::Vec3f center )
	: m_rotation(rotation)
	, m_center(center)
	, m_scale(scale)
	, m_position(position)
{	
}

LayerBase::Transform ::Transform(const Transform& other) = default;

LayerBase::Transform ::~Transform() = default;

LayerBase ::Transform& LayerBase::Transform ::operator=(const Transform& other) = default;



void LayerBase::Transform ::setPosition(const Math::Vec3f& position) {
	m_position = position;
}

const Math::Vec3f& LayerBase::Transform ::getPosition() const {
	return m_position;
}


void LayerBase::Transform ::setRotation(const Math::Quaternionf& rotation) {
	m_rotation = rotation;
}

const Math::Quaternionf& LayerBase::Transform ::getRotation() const {
	return m_rotation;
}


void LayerBase::Transform ::setScale(const Math::Vec3f& scale) {
	m_scale = scale;
}

const Math::Vec3f& LayerBase::Transform ::getScale() const {
	return m_scale;
}


void LayerBase::Transform ::setCenter(const Math::Vec3f& center) {
	m_center = center;
}

const Math::Vec3f& LayerBase::Transform ::getCenter() const {
	return m_center;
}



void LayerBase::Transform ::lookAt(const Math::Vec3f& position, const Math::Vec3f& target, const Math::Vec3f& up) {
	setPosition(position);
	lookAt(target, up);
}

void LayerBase::Transform ::lookAt(const Math::Vec3f& target, const Math::Vec3f& up) {
	const auto direction = target - getPosition();
	setRotation(Math::lookAt(direction, up));
}



Math::Mat4x4f LayerBase::Transform ::calculateModelMatrix() const {
	Math::Mat4x4f result(1.0f); //Load identity

	result = Math::translate(result, -m_center);
	result = Math::scale(result, m_scale);
	result = Math::rotate(result, m_rotation);
	result = Math::translate(result, m_position);

	return result;
}
	
}