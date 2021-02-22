#include "Classifier.h"

#include <zuazo/Math/Approximation.h>

//This code is based on:
//https://opensource.apple.com/source/WebCore/WebCore-1298.39/platform/graphics/gpu/LoopBlinnClassifier.cpp.auto.html
//https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-25-rendering-vector-art-gpu

namespace Zuazo::BlinnLoop {

template<typename T>
constexpr typename Classifier<T>::Result Classifier<T>::operator()(const curve_type& curve) const noexcept{
	Result result;

	//Calculate the mixed product of the control points. Note that we're using
	//a affine space (1.0f at the end of the vector)
	const auto a1 = Math::det(Math::Mat3x3<value_type>( //b0.(b3 x b2)
		Zuazo::Math::Vec3<value_type>(curve[0], 1), 
		Zuazo::Math::Vec3<value_type>(curve[3], 1), 
		Zuazo::Math::Vec3<value_type>(curve[2], 1)
	)); 
	const auto a2 = Math::det(Math::Mat3x3<value_type>( //b1.(b0 x b3)
		Zuazo::Math::Vec3<value_type>(curve[1], 1), 
		Zuazo::Math::Vec3<value_type>(curve[0], 1), 
		Zuazo::Math::Vec3<value_type>(curve[3], 1)
	)); 
	const auto a3 = Math::det(Math::Mat3x3<value_type>( //b2.(b1 x b0)
		Zuazo::Math::Vec3<value_type>(curve[2], 1), 
		Zuazo::Math::Vec3<value_type>(curve[1], 1), 
		Zuazo::Math::Vec3<value_type>(curve[0], 1) //Errata in GPU Gems 3
	));

	//In order to avoid numerical inestability, normaliza the values.
	//This won't affect the result. Idea from Apple's WebCore impl
	const auto D = Math::normalize(Math::Vec3<value_type>(
		a1 - 2*a2 + 3*a3,
		-a2 + 3*a3,
		3*a3
	));

	//Obtain the d values, rounding to zero small values
	result.d1 = Math::approxZero(D.x);
	result.d2 = Math::approxZero(D.y);
	result.d3 = Math::approxZero(D.z);

	if(	!Math::approxZero(Math::length2(curve[0]-curve[1])) && //length2 is cheaper than length
		!Math::approxZero(Math::length2(curve[1]-curve[2])) &&
		!Math::approxZero(Math::length2(curve[2]-curve[3])) )
	{
		result.type = CurveType::POINT;
	} else {
		result.discriminantTerm1 = 3*result.d2*result.d2 - 4*result.d1*result.d3; //3d2^2 - 4d1d3
		const auto discriminant = Math::approxZero(result.d1*result.d1*result.discriminantTerm1); //d1^2 * above

		if(!discriminant) {
			if(!result.d1 && !result.d2) {
				if(!result.d3) {
					//All zero: line
					result.type = CurveType::LINE;
				} else {
					//d1 and d2 = 0, d3 != 0: quadratic
					result.type = CurveType::QUADRATIC;
				}
			} else {
				if(!result.d1) {
					//d=1 but term1 != 0: Special case of cusp
					result.type = CurveType::CUSP;
				} else {
					//disc=0: Cusp, edge case of serpentine or loop
					//As term1 will not be exactly 0, decide between 
					//loop and serpentine to avoid NaN as a result of
					//sqrt. Source: Apple's WebCore
					if(result.discriminantTerm1 < 0) {
						result.type = CurveType::LOOP;
					} else {
						result.type = CurveType::SERPENTINE;
					}
				}
			}
		} else if(discriminant < 0) {
			result.type = CurveType::LOOP;
		} else {
			result.type = CurveType::SERPENTINE;
		}
	}

	assert(static_cast<int>(result.type) >= 0); //Ensure it has been written
	return result;
}

}