#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "color_transfer.glsl"

//Vertex I/O
layout(location = 0) in vec2 ex_texCoord;

layout(location = 0) out vec4 out_color0;
layout(location = 1) out vec4 out_color1;
layout(location = 2) out vec4 out_color2;
layout(location = 3) out vec4 out_color3;

//Uniform buffers
layout(set = 0, binding = 1) uniform OutputColorTransferBlock{
	ct_write_data outColorTransfer;
};

layout(set = 1, binding = 1) uniform LayerDataBlock {
	int sampleMode;
	float opacity;
};

layout(set = 2, binding = ct_SAMPLER_BINDING) uniform sampler2D samplers[ct_SAMPLER_COUNT];
layout(set = 2, binding = ct_DATA_BINDING) uniform InputColorTransferBlock {
	ct_read_data inColorTransfer;
};

void main() {
	//Sample the color from the frame
	vec4 color = ct_texture(sampleMode, inColorTransfer, samplers, ex_texCoord);

	//Perform colorspace conversion
	color = ct_transferColor(inColorTransfer, outColorTransfer, color);

	//Apply the opacity to it
	color.a *= opacity;

	if(color.a == 0.0f) {
		discard;
	} else {
		//Output the data
		ct_output(
			outColorTransfer, color,
			out_color0, out_color1, out_color2, out_color3			
		);
	}
}
 
