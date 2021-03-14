#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "color_transfer.glsl"
#include "frame.glsl"
#include "bezier.glsl"
#include "border.glsl"

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



void main() {
	//Obtain th signed distance to the curve
	const float sDist = bezier3_signed_distance(in_klm);

	//Sample the frame 
	vec4 frameColor = frame_texture(2, in_texCoord);
	frameColor = ct_transferColor(frame_color_transfer(2), outColorTransfer, frameColor);

	//Apply the border to it
	out_color = border_smooth(
		frameColor,			//Fill color
		vec4(0.0f),			//Outter color
		lineColor,			//Border color
		lineWidth,			//Border width
		lineSmoothness,		//Border smoothness
		sDist				//Signed distance to the border
	);

	//Apply the opacity and bezier alpha to it
	out_color.a *= opacity;

	if(out_color.a <= 0.0f) {
		discard;
	} else {
		out_color = ct_premultiply_alpha(out_color);
	}
}
 