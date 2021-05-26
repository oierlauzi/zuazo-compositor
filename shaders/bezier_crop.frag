#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "frame.glsl"
#include "bezier.glsl"
#include "border.glsl"

//Constants
layout (constant_id = 0) const int SAMPLE_MODE = frame_SAMPLE_MODE_PASSTHOUGH;

//Vertex I/O
layout(location = 0) in vec2 in_texCoord;
layout(location = 1) in vec3 in_klm;

layout(location = 0) out vec4 out_color;

//Uniform buffers
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
	if(sDist > 0) {
		discard;
	}

	//Sample the color from the frame
	vec4 color = frame_texture(SAMPLE_MODE, frame_sampler(2), in_texCoord);

	//Apply the opacity and bezier alpha to it
	color.a *= opacity;

	//Premultiply alpha for outputing
	out_color = frame_premultiply_alpha(color);
}
 