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
	float opacity;
};

//Frame descriptor set
frame_descriptor_set(2)

void main() {
	//Obtain th signed distance to the curve
	const float sDist = bezier3_signed_distance(in_klm);

	//Obtain the line factor and the edge factor from the distance
	const float lineFactor = clamp(0.5f - sDist - lineWidth, 0.0f, 1.0f);
	const float edgeFactor = clamp(0.5f - sDist, 0.0f, 1.0f);

	//Sample the color from the frame
	out_color = frame_texture(2, in_texCoord);

	//Perform colorspace conversion
	out_color = ct_transferColor(frame_color_transfer(2), outColorTransfer, out_color);

	//Apply the line color if necessary
	out_color = mix(lineColor, out_color, lineFactor);

	//Apply the opacity and bezier alpha to it
	out_color.a *= opacity * edgeFactor;

	if(out_color.a <= 0.0f) {
		discard;
	} else {
		out_color = ct_premultiply_alpha(out_color);
	}
}
 