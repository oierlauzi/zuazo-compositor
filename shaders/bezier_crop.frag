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
	float opacity;
};

//Frame descriptor set
frame_descriptor_set(2)

void main() {
	//Sample the color from the frame
	vec4 color = frame_texture(2, in_texCoord);

	//Perform colorspace conversion
	color = ct_transferColor(frame_color_transfer(2), outColorTransfer, color);

	//Obtain th signed distance to the curve
	const float sDist = bezier3_signed_distance(in_klm);

	//Apply the opacity and bezier alpha to it
	color.a *= opacity * clamp(0.5f - sDist, 0.0f, 1.0f);

	if(color.a <= 0.0f) {
		discard;
	} else {
		out_color = ct_premultiply_alpha(color);
	}
}
 