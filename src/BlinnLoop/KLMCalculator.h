#pragma once

#include "Classifier.h"

#include <zuazo/Math/Vector.h>

#include <array>

namespace Zuazo::BlinnLoop {

enum class FillSide {
	LEFT,
	RIGHT
};

template<typename T>
struct KLMCalculator {
	using value_type = T;
	using curve_type = typename Classifier<T>::curve_type;
	using classification_type = typename Classifier<T>::Result;
	using klm_type = Math::Vec3<value_type>;

	struct Result {
		std::array<klm_type, curve_type::size()>klmCoords;	
		value_type								subdivisionParameter = std::numeric_limits<value_type>::quiet_NaN();
		bool									isLineOrPoint = false;															
	};

	constexpr Result operator()(const classification_type& c,
								FillSide side ) const noexcept;
};

}

#include "KLMCalculator.inl"