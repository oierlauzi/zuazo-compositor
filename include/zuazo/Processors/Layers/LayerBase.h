#pragma once

#include <zuazo/Math/Vector.h>
#include <zuazo/Math/Quaternion.h>
#include <zuazo/Math/Matrix.h>

namespace Zuazo::Processors::Layers {

class LayerBase {
	class Transform;
};



class LayerBase::Transform {
public:
	Transform(	Math::Vec3f position 		= Math::Vec3f(0.0f, 0.0f, 0.0f),
				Math::Quaternionf rotation	= Math::Quaternionf(0.0f, 0.0f, 0.0f, 0.0f),
				Math::Vec3f scale			= Math::Vec3f(1.0f, 1.0f, 1.0f),
				Math::Vec3f center 			= Math::Vec3f(0.0f, 0.0f, 0.0f) );
	Transform(const Transform& other);
	~Transform();

	Transform& 					operator=(const Transform& other);

	void						setPosition(const Math::Vec3f& position);
	const Math::Vec3f&			getPosition() const;

	void						setRotation(const Math::Quaternionf& rotation);
	const Math::Quaternionf&	getRotation() const;

	void						setScale(const Math::Vec3f& scale);
	const Math::Vec3f&			getScale() const;

	void						setCenter(const Math::Vec3f& center);
	const Math::Vec3f&			getCenter() const;

	void						lookAt(const Math::Vec3f& position, const Math::Vec3f& target, const Math::Vec3f& up);
	void						lookAt(const Math::Vec3f& target, const Math::Vec3f& up);

	Math::Mat4x4f				calculateModelMatrix() const;

private:
	Math::Quaternionf			m_rotation;
	Math::Vec3f					m_center;
	Math::Vec3f					m_scale;
	Math::Vec3f					m_position;

};

}