#version 450

//Vertex I/O
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec2 in_texCoord;
layout(location = 2) in vec3 in_klm;

layout(location = 0) out vec2 out_texCoord;
layout(location = 1) out vec3 out_klm;

//Uniform buffers
layout(set = 0, binding = 0) uniform ProjectionBlock {
	mat4 projectionMtx;
};

layout(set = 1, binding = 0) uniform ModelBlock {
	mat4 modelMtx;
};


void main() {
    gl_Position = projectionMtx * modelMtx * in_position;
	out_texCoord = in_texCoord;
	out_klm = in_klm;
}