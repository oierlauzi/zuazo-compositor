#pragma once

#include <limits>

#include <zuazo/Math/Bezier.h>

namespace Zuazo::BlinnLoop {

enum class CurveType {
	POINT,
	LINE,
	QUADRATIC,
	SERPENTINE,
	CUSP,
	LOOP,
};

template<typename T>
struct Classifier {
	using value_type = T;
	using curve_type = Zuazo::Math::CubicBezier<Zuazo::Math::Vec2<value_type>>;

	struct Result {
		CurveType			type = static_cast<CurveType>(-1);
		value_type			d1 = std::numeric_limits<value_type>::quiet_NaN();
		value_type			d2 = std::numeric_limits<value_type>::quiet_NaN();
		value_type			d3 = std::numeric_limits<value_type>::quiet_NaN();
		value_type			discriminantTerm1 = std::numeric_limits<value_type>::quiet_NaN();
	};

	constexpr Result operator()(const curve_type& curve) const noexcept;

};

}

#include "Classifier.inl"