#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "frame.glsl"

//Constants
layout (constant_id = 0) const int SAMPLE_MODE = frame_SAMPLE_MODE_PASSTHOUGH;

//Vertex I/O
layout(location = 0) in vec2 in_texCoord;

layout(location = 0) out vec4 out_color;

//Uniform buffers
layout(set = 1, binding = 1) uniform LayerDataBlock {
	float opacity;
};

//Frame descriptor set
frame_descriptor_set(2)

void main() {
	//Sample the color from the frame
	vec4 color = frame_texture(SAMPLE_MODE, frame_sampler(2), in_texCoord);

	//Apply the opacity to it
	color.a *= opacity;

	//Premultiply alpha for outputing
	out_color = frame_premultiply_alpha(color);
}
 
