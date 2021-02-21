#include <zuazo/Processors/Layers/BezierCrop.h>

#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/StaticId.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Graphics/StagedBuffer.h>
#include <zuazo/Graphics/UniformBuffer.h>
#include <zuazo/Graphics/CommandBufferPool.h>
#include <zuazo/Graphics/ColorTransfer.h>
#include <zuazo/Math/Geometry.h>
#include <zuazo/Math/Absolute.h>


#include <utility>
#include <memory>
#include <unordered_map>
#include <iostream> //TODO

namespace Zuazo::Processors::Layers {

struct BezierCropImpl {
	struct Open {
		struct Vertex {
			Vertex(	Math::Vec2f position, 
					Math::Vec2f texCoord = Math::Vec2f(0, 0), 
					Math::Vec3f klm = Math::Vec3f(-1, 0, 0) )
				: position(position)
				, texCoord(texCoord)
				, klm(klm)
			{
			}

			Math::Vec2f position;
			Math::Vec2f texCoord;
			Math::Vec3f klm;
		};

		using Index = uint16_t;

		enum VertexLayout {
			VERTEX_LOCATION_POSITION,
			VERTEX_LOCATION_TEXCOORD,
			VERTEX_LOCATION_KLM,

			VERTEX_LOCATION_COUNT
		};

		enum DescriptorSets {
			DESCRIPTOR_SET_RENDERER = RendererBase::DESCRIPTOR_SET,
			DESCRIPTOR_SET_VIDEOSURFACE,
			DESCRIPTOR_SET_FRAME,

			DESCRIPTOR_SET_COUNT
		};

		enum DescriptorBindings {
			DESCRIPTOR_BINDING_MODEL_MATRIX,
			DESCRIPTOR_BINDING_LAYERDATA,

			DESCRIPTOR_COUNT
		};

		enum LayerDataUniforms {
			LAYERDATA_UNIFORM_OPACITY,

			LAYERDATA_UNIFORM_COUNT
		};

		static constexpr std::array<Utils::Area, LAYERDATA_UNIFORM_COUNT> LAYERDATA_UNIFORM_LAYOUT = {
			Utils::Area(0, 	sizeof(float)  	)	//LAYERDATA_UNIFORM_OPACITY
		};

		static constexpr uint32_t VERTEX_BUFFER_BINDING = 0;

		struct Resources {
			Resources(	Graphics::UniformBuffer uniformBuffer,
						vk::UniqueDescriptorPool descriptorPool )
				: vertexBuffer()
				, indexBuffer()
				, uniformBuffer(std::move(uniformBuffer))
				, descriptorPool(std::move(descriptorPool))
			{
			}

			~Resources() = default;

			Graphics::StagedBuffer								vertexBuffer;
			Graphics::StagedBuffer								indexBuffer;
			Graphics::UniformBuffer								uniformBuffer;
			vk::UniqueDescriptorPool							descriptorPool;
		};

		const Graphics::Vulkan&								vulkan;

		std::shared_ptr<Resources>							resources;
		vk::DescriptorSet									descriptorSet;

		vk::DescriptorSetLayout								frameDescriptorSetLayout;
		vk::PipelineLayout									pipelineLayout;
		std::shared_ptr<vk::UniquePipeline>					pipeline;

		Open(	const Graphics::Vulkan& vulkan,
				const BezierCrop::BezierLoop& shape,
				ScalingMode scalingMode,
				const Math::Transformf& transform,
				float opacity ) 
			: vulkan(vulkan)
			, resources(Utils::makeShared<Resources>(	createUniformBuffer(vulkan),
														createDescriptorPool(vulkan) ))
			, descriptorSet(createDescriptorSet(vulkan, *resources->descriptorPool))
			, frameDescriptorSetLayout()
			, pipelineLayout()
			, pipeline()
		{
			resources->uniformBuffer.writeDescirptorSet(vulkan, descriptorSet);
			updateModelMatrixUniform(transform);
			updateOpacityUniform(opacity);
			fillVertexBuffer(shape);
		}

		~Open() {
			resources->vertexBuffer.waitCompletion(vulkan);
			resources->uniformBuffer.waitCompletion(vulkan);
		}

		void recreate(	Graphics::RenderPass renderPass,
						BlendingMode blendingMode ) 
		{
			pipeline = Utils::makeShared<vk::UniquePipeline>(createPipeline(vulkan, pipelineLayout, renderPass, blendingMode));
		}

		void draw(	Graphics::CommandBuffer& cmd, 
					const Video& frame, 
					ScalingFilter filter,
					Graphics::RenderPass renderPass,
					BlendingMode blendingMode ) 
		{				
			assert(resources);			
			assert(frame);

			//Only draw if geometry is defined
			if(resources->indexBuffer.size()) {
				assert(resources->vertexBuffer.size());

				//Update the vertex buffer if needed
				resources->vertexBuffer.waitCompletion(vulkan);
				/*if(geometry.useFrame(*frame)) {
					//Buffer has changed
					resources->vertexBuffer.flushData(
						vulkan,
						vulkan.getGraphicsQueueIndex(),
						vk::AccessFlagBits::eVertexAttributeRead,
						vk::PipelineStageFlagBits::eVertexInput
					);
				}*/

				//Flush the unform buffer
				resources->uniformBuffer.flush(vulkan);

				//Configure the sampler for propper operation
				configureSampler(*frame, filter, renderPass, blendingMode);
				assert(frameDescriptorSetLayout);
				assert(pipelineLayout);
				assert(pipeline);
				assert(*pipeline);

				//Bind the pipeline and its descriptor sets
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

				cmd.bindVertexBuffers(
					VERTEX_BUFFER_BINDING,											//Binding
					resources->vertexBuffer.getBuffer(),							//Vertex buffers
					0UL																//Offsets
				);

				cmd.bindIndexBuffer(
					resources->indexBuffer.getBuffer(),								//Index buffer
					0,																//Offset
					vk::IndexType::eUint16											//Index type
				);

				cmd.bindDescriptorSets(
					vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
					pipelineLayout,													//Pipeline layout
					DESCRIPTOR_SET_VIDEOSURFACE,									//First index
					descriptorSet,													//Descriptor sets
					{}																//Dynamic offsets
				);

				frame->bind(
					cmd.get(), 														//Commandbuffer
					pipelineLayout, 												//Pipeline layout
					DESCRIPTOR_SET_FRAME, 											//Descriptor set index
					filter															//Filter
				);

				//Draw the frame and finish recording
				cmd.drawIndexed(
					resources->indexBuffer.size() / sizeof(Index),					//Index count
					1, 																//Instance count
					0, 																//First index
					0, 																//First vertex
					0																//First instance
				);

				//Add the dependencies to the command buffer
				cmd.addDependencies({ resources, pipeline, frame });
			}		
		}

		void updateModelMatrixUniform(const Math::Transformf& transform) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);

			const auto mtx = transform.calculateMatrix();
			resources->uniformBuffer.write(
				vulkan,
				DESCRIPTOR_BINDING_MODEL_MATRIX,
				&mtx,
				sizeof(mtx)
			);
		}

		void updateOpacityUniform(float opa) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			resources->uniformBuffer.write(
				vulkan,
				DESCRIPTOR_BINDING_LAYERDATA,
				&opa,
				sizeof(opa),
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_OPACITY].offset()
			);
		}

	private:
		void configureSampler(	const Graphics::Frame& frame, 
								ScalingFilter filter,
								Graphics::RenderPass renderPass,
								BlendingMode blendingMode ) 
		{
			const auto newDescriptorSetLayout = frame.getDescriptorSetLayout(filter);
			if(frameDescriptorSetLayout != newDescriptorSetLayout) {
				frameDescriptorSetLayout = newDescriptorSetLayout;
				pipelineLayout = createPipelineLayout(vulkan, frameDescriptorSetLayout);
				recreate(renderPass, blendingMode);
			}
		}

		void fillVertexBuffer(const BezierCrop::BezierLoop& loop) {
			constexpr Index PRIMITIVE_RESTART_INDEX = ~Index(0);
			//Obtain the vertices in the inner hull
			std::vector<Math::Vec2f> innerHull;
			for(size_t i = 0; i < loop.segmentCount(); ++i) {
				const auto& segment = loop.getSegment(i);

				//First point will always be at the inner hull
				innerHull.emplace_back(segment.front());

				//Evaluate it for the 2 control points
				for(size_t j = 1; j < segment.degree(); ++j) {
					const auto signedDistance = Math::getSignedDistance(
						Math::Line<float, 2>(segment.front(), segment.back()),
						segment[j]
					);

					if(signedDistance < 0) {
						//This segment lies on the inside
						innerHull.emplace_back(segment[j]);
					}
				}
			}

			//Triangulate the vertices
			std::vector<Index> indices = Math::triangulate<float, Index>(innerHull); //TODO: vertex to position
			std::vector<Vertex> vertices = std::vector<Vertex>(innerHull.cbegin(), innerHull.cend());

			//Add the outline vertices
			for(size_t i = 0; i < loop.segmentCount(); ++i) {
				const auto& segment = loop.getSegment(i);

				//Based on:
				//https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-25-rendering-vector-art-gpu
				//Calculate the mixed product of the control points. Note that we're using
				//a affine space (1.0f at the end of the vector)
				const auto a1 = Math::det(Math::Mat3x3f( //b0.(b3 x b2)
					Zuazo::Math::Vec3f(segment[0], 1.0f), 
					Zuazo::Math::Vec3f(segment[3], 1.0f), 
					Zuazo::Math::Vec3f(segment[2], 1.0f)
				)); 
				const auto a2 = Math::det(Math::Mat3x3f( //b1.(b0 x b3)
					Zuazo::Math::Vec3f(segment[1], 1.0f), 
					Zuazo::Math::Vec3f(segment[0], 1.0f), 
					Zuazo::Math::Vec3f(segment[3], 1.0f)
				)); 
				const auto a3 = Math::det(Math::Mat3x3f( //b2.(b1 x b0)
					Zuazo::Math::Vec3f(segment[2], 1.0f), 
					Zuazo::Math::Vec3f(segment[1], 1.0f), 
					Zuazo::Math::Vec3f(segment[0], 1.0f)
				)); 

				const auto d1 = a1 - 2*a2 + 3*a3;
				const auto d2 = -a2 + 3*a3; //Maybe -2*?
				const auto d3 = 3*a3;

				//Optimize away lines. They are already defined by the inner hull
				constexpr auto LINE_BIAS = 1e-6;
				if(Math::abs(d1)>LINE_BIAS || Math::abs(d2)>LINE_BIAS || Math::abs(d3)>LINE_BIAS) {
					std::array<Zuazo::Math::Vec3f, segment.size()> klmCoords;
					bool reverse = false;
					constexpr Math::Vec3f REVERSE_COEFF(-1, -1, +1);

					//Optimize away quadratic curves
					if(Math::abs(d1)>LINE_BIAS || Math::abs(d2)>LINE_BIAS) {
						//This is a cubic curve. Decide the type based on the discriminator
						const auto disc = d1*d1*(3*d2*d2 - 4*d1*d3); //d1^2(3d2^2 - 4d1d3)

						if(Math::abs(disc)<LINE_BIAS && Math::abs(d2)>LINE_BIAS) {
							//Special case of a cusp where d2 != 0
							const Math::Vec2f L(d3, 3*d2); //M=L
							const auto smt = L.x - L.y;

							klmCoords = {
								Math::Vec3f(
									L.x,											//ls, 
									L.x*L.x*L.x,									//ls^3
									1.0f											//1
								),
								Math::Vec3f(
									L.x - 1*L.y/3,									//ls - 1/3*lt
									L.x*L.x*smt,									//ls^2*(ls-lt)
									1.0f											//1
								), 
								Math::Vec3f(
									L.x - 2*L.y/3,									//ls - 2/3*lt
									L.x*smt*smt,									//ls*(ls-lt)^2
									1.0f											//1
								),	
								Math::Vec3f(
									smt,											//ls-lt	
									smt*smt*smt,									//(ls-lt)^3
									1.0f											//1
								) 
							};
							reverse = false; //Never reverses

						} else if(disc >= 0) {
							//Serpentine
							const auto sqrtDisc = Math::sqrt(9*d2*d2 - 12*d1*d3); //Ensured to be real, as disc>=3
							const Math::Vec2f L(3*d2 - sqrtDisc, 6*d1);
							const Math::Vec2f M(3*d2 + sqrtDisc, 6*d1);
							const auto Lsmt = L.x - L.y;
							const auto Msmt = M.x - M.y;

							klmCoords = {
								Math::Vec3f(
									L.x*M.x,										//ls*ms, 			
									L.x*L.x*L.x,									//ls^3
									M.x*M.x*M.x										//ms^3
								), 
								Math::Vec3f(
									(3*L.x*M.x - L.x*M.y - L.y*M.x)/3, 				//ls*ms - 1/3*ls*mt - 1/3*lt*ms
									L.x*L.x*Lsmt,									//ls^2*(ls-lt) 
									M.x*M.x*Msmt									//ms^2*(ms-mt)
								), 			
								Math::Vec3f(
									(3*L.x*M.x - 2*L.x*M.y - 2*L.y*M.x + L.y*M.y)/3,//ls*ms - 2/3*ls*mt - 2/3*lt*ms - 1/3*lt*mt
									L.x*Lsmt*Lsmt, 									//ls*(ls-lt)^2
									M.x*Msmt*Msmt									//ms*(ms-mt)^2
								), 
								Math::Vec3f(
									Lsmt*Msmt, 										//(ls-lt)*(ms-mt)
									Lsmt*Lsmt*Lsmt, 								//(ls-lt)^3
									Msmt*Msmt*Msmt									//(ms-mt)^3
								)
							};
							reverse = d1 < 0;

						} else {
							//Loop
							const auto sqrtDisc = Math::sqrt(4*d1*d3 - 3*d2*d2); //Ensured to be real, as disc<0
							const Math::Vec2f L(d2 - sqrtDisc, 2*d1);
							const Math::Vec2f M(d2 + sqrtDisc, 2*d1);
							const auto Lsmt = L.x - L.y;
							const auto Msmt = M.x - M.y;

							//TODO check if double point
							klmCoords = {
								Math::Vec3f(
									L.x*M.x,										//ls*ms
									L.x*L.x*M.x,									//ls^2*ms
									L.x*M.x*M.x										//ls*ms^2
								),		 			
								Math::Vec3f(
									(L.x*M.x - L.x*M.y - L.y*M.x)/3,				//1/3*ls*ms - 1/3*ls*mt - 1/3*lt*ms
									(3*L.x*L.x*M.x - 2*L.x*L.y*M.x - L.x*L.x*M.y)/3,//ls^2*ms - 2/3*ls*lt*ms - 1/3*ls^2*mt
									(3*L.x*M.x*M.x - 2*L.x*M.x*M.y - L.y*M.x*M.x)/3	//ls*ms^2 - 2/3*ls*ms*mt - 1/3*lt*ms^2
								), 
								Math::Vec3f(
									(3*L.x*M.x - 2*L.x*M.y - 2*L.y*M.x + L.y*M.y)/3,//ls*ms - 2/3*ls*mt - 2/3*lt*ms + 1/3*lt*mt
									Lsmt*(3*L.x*M.x - 2*L.x*M.y - L.y*M.x)/3, 		//(ls-lt)*(ls*ms - 2/3*ls*mt - 1/3*lt*ms)
									Msmt*(3*L.x*M.x - 2*L.y*M.x - L.x*M.y)/3 		//(ms-mt)*(ls*ms - 2/3*lt*ms - 1/3*ls*mt)
								),
								Math::Vec3f(
									Lsmt*Msmt,										//(ls-lt)*(ms-mt) 
									Lsmt*Lsmt*Msmt,									//(ls-lt)^2*(ms-mt) 
									Lsmt*Msmt*Msmt									//(ls-lt)*(ms-mt)^2
								)
							};
							reverse = Math::sign(klmCoords[1].x) != Math::sign(d1);
						}

					} else {
						//This is a quadratic curve
						//All quadratic curves share the same klm values
						klmCoords = {
							Zuazo::Math::Vec3f(0.0f, 		0.0f, 		0.0f		),
							Zuazo::Math::Vec3f(1.0f/3.0f, 	0.0f, 		1.0f/3.0f	),
							Zuazo::Math::Vec3f(2.0f/3.0f, 	1.0f/3.0f, 	2.0f/3.0f	),
							Zuazo::Math::Vec3f(1.0f, 		1.0f, 		1.0f		)
						};
						reverse = d3 < 0;
					}

					//Something is going to be drawn, reset the primitive assembly
					indices.emplace_back(PRIMITIVE_RESTART_INDEX);

					constexpr std::array<size_t, segment.size()> TRIANGLE_STRIP_MAPPING = {0, 1, 3, 2};

					//Create the vertices and its corresponding indices
					static_assert(segment.size() == klmCoords.size(), "Sizes must match");
					static_assert(segment.size() == TRIANGLE_STRIP_MAPPING.size(), "Sizes must match");
					for(size_t j = 0; j < segment.size(); ++j) {
						//Simply refer to the following vertex
						indices.emplace_back(vertices.size()); 

						//Obtain the components of the vertex and add them to the vertex list
						const auto index = TRIANGLE_STRIP_MAPPING[j];
						const auto& position = segment[index];
						const auto& klmCoord = klmCoords[index];
						std::cout << klmCoord << (j==3 ? "\n" : " ");
						vertices.emplace_back(
							position, 
							Zuazo::Math::Vec2f(), 
							reverse ? REVERSE_COEFF*klmCoord : klmCoord
						);
					}
				}
			}

			if(vertices.size() >= std::numeric_limits<Index>::max()) {
				//Could be removed by increasing to 32bit indices
				throw Exception("Too many vertices for the index precision");
			}

			//Copy the data to the buffers
			assert(resources);
			if(resources->vertexBuffer.size() != vertices.size()*sizeof(Vertex)) {
				resources->vertexBuffer = createVertexBuffer(vulkan, vertices.size());
			}
			assert(resources->vertexBuffer.size() == vertices.size()*sizeof(Vertex));
			std::memcpy(resources->vertexBuffer.data(), vertices.data(), vertices.size()*sizeof(Vertex));
			resources->vertexBuffer.flushData(
				vulkan, 
				vulkan.getTransferQueueIndex(), 
				vk::AccessFlagBits::eVertexAttributeRead,
				vk::PipelineStageFlagBits::eVertexInput
			); //TODO force flushing

			if(resources->indexBuffer.size() != indices.size()*sizeof(Index)) {
				resources->indexBuffer = createIndexBuffer(vulkan, indices.size());
			}
			assert(resources->indexBuffer.size() == indices.size()*sizeof(Index));
			std::memcpy(resources->indexBuffer.data(), indices.data(), indices.size()*sizeof(Index));
			resources->indexBuffer.flushData(
				vulkan, 
				vulkan.getTransferQueueIndex(), 
				vk::AccessFlagBits::eIndexRead,
				vk::PipelineStageFlagBits::eVertexInput
			);
		}


		static Graphics::StagedBuffer createVertexBuffer(const Graphics::Vulkan& vulkan, size_t vertexCount) {
			return Graphics::StagedBuffer(
				vulkan,
				vk::BufferUsageFlagBits::eVertexBuffer,
				sizeof(Vertex) * vertexCount
			);
		}

		static Graphics::StagedBuffer createIndexBuffer(const Graphics::Vulkan& vulkan, size_t indexCount) {
			return Graphics::StagedBuffer(
				vulkan,
				vk::BufferUsageFlagBits::eIndexBuffer,
				sizeof(Index) * indexCount
			);
		}

		static vk::DescriptorSetLayout getDescriptorSetLayout(	const Graphics::Vulkan& vulkan) 
		{
			static const Utils::StaticId id;
			auto result = vulkan.createDescriptorSetLayout(id);

			if(!result) {
				//Create the bindings
				const std::array bindings = {
					vk::DescriptorSetLayoutBinding(	//UBO binding
						DESCRIPTOR_BINDING_MODEL_MATRIX,				//Binding
						vk::DescriptorType::eUniformBuffer,				//Type
						1,												//Count
						vk::ShaderStageFlagBits::eVertex,				//Shader stage
						nullptr											//Immutable samplers
					), 
					vk::DescriptorSetLayoutBinding(	//UBO binding
						DESCRIPTOR_BINDING_LAYERDATA,					//Binding
						vk::DescriptorType::eUniformBuffer,				//Type
						1,												//Count
						vk::ShaderStageFlagBits::eFragment,				//Shader stage
						nullptr											//Immutable samplers
					), 
				};

				const vk::DescriptorSetLayoutCreateInfo createInfo(
					{},
					bindings.size(), bindings.data()
				);

				result = vulkan.createDescriptorSetLayout(id, createInfo);
			}

			return result;
		}

		static Utils::BufferView<const std::pair<uint32_t, size_t>> getUniformBufferSizes() noexcept {
			static const std::array uniformBufferSizes = {
				std::make_pair<uint32_t, size_t>(DESCRIPTOR_BINDING_MODEL_MATRIX, 	sizeof(glm::mat4) ),
				std::make_pair<uint32_t, size_t>(DESCRIPTOR_BINDING_LAYERDATA,		LAYERDATA_UNIFORM_LAYOUT.back().end() )
			};

			return uniformBufferSizes;
		}

		static Graphics::UniformBuffer createUniformBuffer(const Graphics::Vulkan& vulkan) {
			return Graphics::UniformBuffer(vulkan, getUniformBufferSizes());
		}

		static vk::UniqueDescriptorPool createDescriptorPool(const Graphics::Vulkan& vulkan){
			const std::array poolSizes = {
				vk::DescriptorPoolSize(
					vk::DescriptorType::eUniformBuffer,					//Descriptor type
					getUniformBufferSizes().size()						//Descriptor count
				)
			};

			const vk::DescriptorPoolCreateInfo createInfo(
				{},														//Flags
				1,														//Descriptor set count
				poolSizes.size(), poolSizes.data()						//Pool sizes
			);

			return vulkan.createDescriptorPool(createInfo);
		}

		static vk::DescriptorSet createDescriptorSet(	const Graphics::Vulkan& vulkan,
														vk::DescriptorPool pool )
		{
			const auto layout = getDescriptorSetLayout(vulkan);
			return vulkan.allocateDescriptorSet(pool, layout).release();
		}

		static vk::PipelineLayout createPipelineLayout(	const Graphics::Vulkan& vulkan,
														vk::DescriptorSetLayout frameDescriptorSetLayout ) 
		{
			static std::unordered_map<vk::DescriptorSetLayout, const Utils::StaticId> ids;
			const auto& id = ids[frameDescriptorSetLayout]; //TODO make it thread safe

			auto result = vulkan.createPipelineLayout(id);
			if(!result) {
				const std::array layouts = {
					RendererBase::getDescriptorSetLayout(vulkan), 			//DESCRIPTOR_SET_RENDERER
					getDescriptorSetLayout(vulkan), 						//DESCRIPTOR_SET_VIDEOSURFACE
					frameDescriptorSetLayout 								//DESCRIPTOR_SET_FRAME
				};

				const vk::PipelineLayoutCreateInfo createInfo(
					{},													//Flags
					layouts.size(), layouts.data(),						//Descriptor set layouts
					0, nullptr											//Push constants
				);

				result = vulkan.createPipelineLayout(id, createInfo);
			}

			return result;
		}

		static vk::UniquePipeline createPipeline(	const Graphics::Vulkan& vulkan,
													vk::PipelineLayout layout,
													Graphics::RenderPass renderPass,
													BlendingMode blendingMode )
		{
			static //So that its ptr can be used as an identifier
			#include <bezier_crop_vert.h>
			const size_t vertId = reinterpret_cast<uintptr_t>(bezier_crop_vert);
			static
			#include <bezier_crop_frag.h>
			const size_t fragId = reinterpret_cast<uintptr_t>(bezier_crop_frag);

			//Try to retrive modules from cache
			auto vertexShader = vulkan.createShaderModule(vertId);
			if(!vertexShader) {
				//Modules isn't in cache. Create it
				vertexShader = vulkan.createShaderModule(vertId, bezier_crop_vert);
			}

			auto fragmentShader = vulkan.createShaderModule(fragId);
			if(!fragmentShader) {
				//Modules isn't in cache. Create it
				fragmentShader = vulkan.createShaderModule(fragId, bezier_crop_frag);
			}

			assert(vertexShader);
			assert(fragmentShader);

			constexpr auto SHADER_ENTRY_POINT = "main";
			const std::array shaderStages = {
				vk::PipelineShaderStageCreateInfo(		
					{},												//Flags
					vk::ShaderStageFlagBits::eVertex,				//Shader type
					vertexShader,									//Shader handle
					SHADER_ENTRY_POINT ),							//Shader entry point
				vk::PipelineShaderStageCreateInfo(		
					{},												//Flags
					vk::ShaderStageFlagBits::eFragment,				//Shader type
					fragmentShader,									//Shader handle
					SHADER_ENTRY_POINT ),							//Shader entry point
			};

			constexpr std::array vertexBindings = {
				vk::VertexInputBindingDescription(
					VERTEX_BUFFER_BINDING,
					sizeof(Vertex),
					vk::VertexInputRate::eVertex
				)
			};

			constexpr std::array vertexAttributes = {
				vk::VertexInputAttributeDescription(
					VERTEX_LOCATION_POSITION,
					VERTEX_BUFFER_BINDING,
					vk::Format::eR32G32Sfloat,
					offsetof(Vertex, position)
				),
				vk::VertexInputAttributeDescription(
					VERTEX_LOCATION_TEXCOORD,
					VERTEX_BUFFER_BINDING,
					vk::Format::eR32G32Sfloat,
					offsetof(Vertex, texCoord)
				),
				vk::VertexInputAttributeDescription(
					VERTEX_LOCATION_KLM,
					VERTEX_BUFFER_BINDING,
					vk::Format::eR32G32Sfloat,
					offsetof(Vertex, klm)
				)
			};

			const vk::PipelineVertexInputStateCreateInfo vertexInput(
				{},
				vertexBindings.size(), vertexBindings.data(),		//Vertex bindings
				vertexAttributes.size(), vertexAttributes.data()	//Vertex attributes
			);

			constexpr vk::PipelineInputAssemblyStateCreateInfo inputAssembly(
				{},													//Flags
				vk::PrimitiveTopology::eTriangleStrip,				//Topology
				true												//Restart enable
			);

			constexpr vk::PipelineViewportStateCreateInfo viewport(
				{},													//Flags
				1, nullptr,											//Viewports (dynamic)
				1, nullptr											//Scissors (dynamic)
			);

			constexpr vk::PipelineRasterizationStateCreateInfo rasterizer(
				{},													//Flags
				false, 												//Depth clamp enabled
				false,												//Rasterizer discard enable
				vk::PolygonMode::eFill,								//Polygon mode
				vk::CullModeFlagBits::eNone, 						//Cull faces
				vk::FrontFace::eClockwise,							//Front face direction
				false, 0.0f, 0.0f, 0.0f,							//Depth bias
				1.0f												//Line width
			);

			constexpr vk::PipelineMultisampleStateCreateInfo multisample(
				{},													//Flags
				vk::SampleCountFlagBits::e1,						//Sample count
				false, 1.0f,										//Sample shading enable, min sample shading
				nullptr,											//Sample mask
				false, false										//Alpha to coverage, alpha to 1 enable
			);

			constexpr vk::PipelineDepthStencilStateCreateInfo depthStencil(
				{},													//Flags
				true, true, 										//Depth test enable, write
				vk::CompareOp::eLessOrEqual,						//Depth compare op
				false,												//Depth bounds test
				false, 												//Stencil enabled
				{}, {},												//Stencil operation state front, back
				0.0f, 0.0f											//min, max depth bounds
			);

			const std::array colorBlendAttachments = {
				Graphics::toVulkan(blendingMode)
			};

			const vk::PipelineColorBlendStateCreateInfo colorBlend(
				{},													//Flags
				false,												//Enable logic operation
				vk::LogicOp::eCopy,									//Logic operation
				colorBlendAttachments.size(), colorBlendAttachments.data() //Blend attachments
			);

			constexpr std::array dynamicStates = {
				vk::DynamicState::eViewport,
				vk::DynamicState::eScissor
			};

			const vk::PipelineDynamicStateCreateInfo dynamicState(
				{},													//Flags
				dynamicStates.size(), dynamicStates.data()			//Dynamic states
			);

			static const Utils::StaticId pipelineId;
			const vk::GraphicsPipelineCreateInfo createInfo(
				{},													//Flags
				shaderStages.size(), shaderStages.data(),			//Shader stages
				&vertexInput,										//Vertex input
				&inputAssembly,										//Vertex assembly
				nullptr,											//Tesselation
				&viewport,											//Viewports
				&rasterizer,										//Rasterizer
				&multisample,										//Multisampling
				&depthStencil,										//Depth / Stencil tests
				&colorBlend,										//Color blending
				&dynamicState,										//Dynamic states
				layout,												//Pipeline layout
				renderPass.get(), 0,								//Renderpasses
				nullptr, static_cast<uint32_t>(pipelineId)			//Inherit
			);

			return vulkan.createGraphicsPipeline(createInfo);
		}

	};

	using Input = Signal::Input<Video>;
	using LastFrames = std::unordered_map<const RendererBase*, Video>;

	std::reference_wrapper<BezierCrop>		owner;

	Input									videoIn;

	BezierCrop::BezierLoop					shape;

	std::unique_ptr<Open>					opened;
	LastFrames								lastFrames;
	

	BezierCropImpl(BezierCrop& owner, BezierCrop::BezierLoop shape)
		: owner(owner)
		, videoIn()
		, shape(std::move(shape))
	{
	}

	~BezierCropImpl() = default;

	void moved(ZuazoBase& base) {
		owner = static_cast<BezierCrop&>(base);
	}

	void open(ZuazoBase& base) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface);
		assert(!opened);

		if(videoSurface.getRenderPass() != Graphics::RenderPass()) {
			opened = Utils::makeUnique<Open>(
					videoSurface.getInstance().getVulkan(),
					getShape(),
					videoSurface.getScalingMode(),
					videoSurface.getTransform(),
					videoSurface.getOpacity()
			);
		}

		assert(lastFrames.empty()); //Any hasChanged() should return true
	}

	void asyncOpen(ZuazoBase& base, std::unique_lock<Instance>& lock) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface);
		assert(!opened);
		assert(lock.owns_lock());

		if(videoSurface.getRenderPass() != Graphics::RenderPass()) {
			lock.unlock();
			auto newOpened = Utils::makeUnique<Open>(
					videoSurface.getInstance().getVulkan(),
					getShape(),
					videoSurface.getScalingMode(),
					videoSurface.getTransform(),
					videoSurface.getOpacity()
			);
			lock.lock();

			opened = std::move(newOpened);
		}

		assert(lastFrames.empty()); //Any hasChanged() should return true
		assert(lock.owns_lock());
	}


	void close(ZuazoBase& base) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);
		
		videoIn.reset();
		lastFrames.clear();
		opened.reset();

		assert(!opened);
	}

	void asyncClose(ZuazoBase& base, std::unique_lock<Instance>& lock) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);
		assert(lock.owns_lock());
		
		videoIn.reset();
		lastFrames.clear();
		auto oldOpened = std::move(opened);

		if(oldOpened) {
			lock.unlock();
			oldOpened.reset();
			lock.lock();
		}

		assert(!opened);
		assert(lock.owns_lock());
	}

	bool hasChangedCallback(const LayerBase& base, const RendererBase& renderer) const {
		const auto& videoSurface = static_cast<const BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		const auto ite = lastFrames.find(&renderer);
		if(ite == lastFrames.cend()) {
			//There is no frame previously rendered for this renderer
			return true;
		}

		if(ite->second != videoIn.getLastElement()) {
			//A new frame has arrived since the last rendered one at this renderer
			return true;
		}

		if(videoIn.hasChanged()) {
			//A new frame is available
			return true;
		}

		//Nothing has changed :-)
		return false;
	}

	void drawCallback(const LayerBase& base, const RendererBase& renderer, Graphics::CommandBuffer& cmd) {
		const auto& videoSurface = static_cast<const BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			const auto& frame = videoIn.pull();
			
			//Draw
			if(frame) {
				opened->draw(
					cmd, 
					frame, 
					videoSurface.getScalingFilter(),
					videoSurface.getRenderPass(),
					videoSurface.getBlendingMode()
				);
			}

			//Update the state for next hasChanged()
			lastFrames[&renderer] = frame;
		}
	}

	void transformCallback(LayerBase& base, const Math::Transformf& transform) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->updateModelMatrixUniform(transform);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void opacityCallback(LayerBase& base, float opa) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->updateOpacityUniform(opa);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void blendingModeCallback(LayerBase& base, BlendingMode mode) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		recreateCallback(videoSurface, videoSurface.getRenderPass(), mode);
	}

	void renderPassCallback(LayerBase& base, Graphics::RenderPass renderPass) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		recreateCallback(videoSurface, renderPass, videoSurface.getBlendingMode());
	}

	void scalingModeCallback(VideoScalerBase& base, ScalingMode mode) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		//TODO

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void scalingFilterCallback(VideoScalerBase& base, ScalingFilter) {
		auto& videoSurface = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		lastFrames.clear(); //Will force hasChanged() to true
	}


	void setShape(BezierCrop::BezierLoop shape) {
		this->shape = std::move(shape);
	}

	const BezierCrop::BezierLoop& getShape() const {
		return shape;
	}

private:
	void recreateCallback(	BezierCrop& bezierCrop, 
							Graphics::RenderPass renderPass,
							BlendingMode blendingMode )
	{
		assert(&owner.get() == &bezierCrop);

		if(bezierCrop.isOpen()) {
			const bool isValid = 	renderPass != Graphics::RenderPass() &&
									blendingMode > BlendingMode::NONE ;

			if(opened && isValid) {
				//It remains valid
				opened->recreate(
					renderPass,
					blendingMode
				);
			} else if(opened && !isValid) {
				//It has become invalid
				videoIn.reset();
				opened.reset();
			} else if(!opened && isValid) {
				//It has become valid
				opened = Utils::makeUnique<Open>(
					bezierCrop.getInstance().getVulkan(),
					getShape(),
					bezierCrop.getScalingMode(),
					bezierCrop.getTransform(),
					bezierCrop.getOpacity()
				);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

};




BezierCrop::BezierCrop(	Instance& instance,
						std::string name,
						const RendererBase* renderer,
						BezierLoop shape )
	: Utils::Pimpl<BezierCropImpl>({}, *this, std::move(shape))
	, ZuazoBase(
		instance, 
		std::move(name),
		{ PadRef((*this)->videoIn) },
		std::bind(&BezierCropImpl::moved, std::ref(**this), std::placeholders::_1),
		std::bind(&BezierCropImpl::open, std::ref(**this), std::placeholders::_1),
		std::bind(&BezierCropImpl::asyncOpen, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::close, std::ref(**this), std::placeholders::_1),
		std::bind(&BezierCropImpl::asyncClose, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, LayerBase(
		renderer,
		std::bind(&BezierCropImpl::transformCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::opacityCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::blendingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::hasChangedCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::drawCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
		std::bind(&BezierCropImpl::renderPassCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, VideoScalerBase(
		std::bind(&BezierCropImpl::scalingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::scalingFilterCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, Signal::ConsumerLayout<Video>(makeProxy((*this)->videoIn))
{
}

BezierCrop::BezierCrop(BezierCrop&& other) = default;

BezierCrop::~BezierCrop() = default;

BezierCrop& BezierCrop::operator=(BezierCrop&& other) = default;


void BezierCrop::setShape(BezierLoop shape) {
	(*this)->setShape(std::move(shape));
}

const BezierCrop::BezierLoop& BezierCrop::getShape() const {
	return (*this)->getShape();
}

}