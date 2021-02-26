#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "color_transfer.glsl"
#include "frame.glsl"
#include "bezier.glsl"

//Vertex I/O
layout(location = 0) in vec2 in_texCoord;
layout(location = 1) in vec3 in_klm;

layout(location = 0) out vec4 out_color;

//Uniform buffers
layout(set = 0, binding = 1) uniform OutputColorTransferBlock{
	ct_write_data outColorTransfer;
};

layout(set = 1, binding = 1) uniform LayerDataBlock {
	vec4 lineColor;
	float lineWidth;
	float lineSmoothness;
	float opacity;
};

//Frame descriptor set
frame_descriptor_set(2)

//Constants
const vec4 voidColor = vec4(0.0f);

void main() {
	//Obtain th signed distance to the curve
	const float sDist = bezier3_signed_distance(in_klm);

	//Obtain the blend coefficients
	const float blend0 = clamp(0.5f + sDist / lineSmoothness, 0.0f, 1.0f); //Void gain
	const float blend1 = clamp(0.5f - (sDist + lineWidth) / lineSmoothness, 0.0f, 1.0f); //Frame gain
	const float blend2 = 1.0f - blend0 - blend1; //Line gain

	vec4 frameColor;
	if(blend1 > 0.0f) {
		//Sample the color from the frame, as it will
		//be visible
		frameColor = frame_texture(2, in_texCoord);

		//Perform colorspace conversion
		frameColor = ct_transferColor(frame_color_transfer(2), outColorTransfer, frameColor);
	}

	//Obtain the resulting color
	out_color = blend0*voidColor + blend1*frameColor + blend2*lineColor;

	//Apply the opacity and bezier alpha to it
	out_color.a *= opacity;

	if(out_color.a <= 0.0f) {
		discard;
	} else {
		out_color = ct_premultiply_alpha(out_color);
	}
}
 