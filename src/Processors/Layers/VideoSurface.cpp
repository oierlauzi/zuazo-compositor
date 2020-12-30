#include <zuazo/Processors/Layers/VideoSurface.h>

#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/StaticId.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Graphics/StagedBuffer.h>
#include <zuazo/Graphics/CommandBufferPool.h>
#include <zuazo/Graphics/ColorTransfer.h>

#include <utility>
#include <memory>
#include <unordered_map>

namespace Zuazo::Processors::Layers {

struct VideoSurfaceImpl {
	struct Open {
		struct Vertex {
			Math::Vec2f position;
			Math::Vec2f texCoord;
		};

		enum VertexLayout {
			VERTEX_POSITION,
			VERTEX_TEXCOORD,

			VERTEX_COUNT
		};

		enum DescriptorSets {
			DESCRIPTOR_SET_RENDERER,
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
			LAYERDATA_UNIFORM_SAMPLEMODE,
			LAYERDATA_UNIFORM_OPACITY,

			LAYERDATA_UNIFORM_COUNT
		};

		static constexpr std::array<Utils::Area, LAYERDATA_UNIFORM_COUNT> LAYERDATA_UNIFORM_LAYOUT = {
			Utils::Area(0, 					sizeof(int32_t)	), 	//LAYERDATA_UNIFORM_SAMPLEMODE
			Utils::Area(sizeof(int32_t), 	sizeof(float)  	)	//LAYERDATA_UNIFORM_OPACITY
		};


		static constexpr uint32_t VERTEX_BUFFER_BINDING = 0;
		using UniformBufferLayout = std::array<Utils::Area, DESCRIPTOR_COUNT>;

		struct Resources {
			Resources(	Graphics::StagedBuffer vertexBuffer,
						Graphics::StagedBuffer uniformBuffer,
						vk::UniqueDescriptorPool descriptorPool )
				: vertexBuffer(std::move(vertexBuffer))
				, uniformBuffer(std::move(uniformBuffer))
				, descriptorPool(std::move(descriptorPool))
			{
			}

			~Resources() = default;

			Graphics::StagedBuffer								vertexBuffer;
			Graphics::StagedBuffer								uniformBuffer;
			vk::UniqueDescriptorPool							descriptorPool;
		};

		const Graphics::Vulkan&								vulkan;

		UniformBufferLayout									uniformBufferLayout;
		std::shared_ptr<Resources>							resources;
		Graphics::Frame::Geometry							geometry;
		Graphics::InputColorTransfer::SamplerDescriptor		samplerDesc;
		vk::DescriptorSet									descriptorSet;

		vk::PipelineLayout									pipelineLayout;
		std::shared_ptr<vk::UniquePipeline>					pipeline;

		Math::Mat4x4f&										uniformModelMatrix;
		int32_t&											uniformSampleMode;
		float&												uniformOpacity;
		Utils::Area											uniformFlushArea;
		vk::PipelineStageFlags								uniformFlushStages;

		Open(	const Graphics::Vulkan& vulkan,
				Math::Vec2f size,
				ScalingMode scalingMode,
				ScalingFilter scalingFilter,
				Graphics::RenderPass renderPass,
				BlendingMode blendingMode,
				const Math::Transformf& transform,
				float opacity ) 
			: vulkan(vulkan)
			, uniformBufferLayout(createUniformBufferLayout(vulkan))
			, resources(Utils::makeShared<Resources>(	createVertexBuffer(vulkan),
														createUniformBuffer(vulkan, uniformBufferLayout),
														createDescriptorPool(vulkan) ))
			, geometry(resources->vertexBuffer.data(), sizeof(Vertex), offsetof(Vertex, position), offsetof(Vertex, texCoord), scalingMode, size)
			, samplerDesc(Graphics::InputColorTransfer::getSamplerDescriptor(scalingFilter))
			, descriptorSet(createDescriptorSet(vulkan, *resources->descriptorPool))
			, pipelineLayout(createPipelineLayout(vulkan, samplerDesc.filter))
			, pipeline(Utils::makeShared<vk::UniquePipeline>(createPipeline(vulkan, pipelineLayout, renderPass, blendingMode)))
			, uniformModelMatrix(getModelMatrix(uniformBufferLayout, resources->uniformBuffer))
			, uniformSampleMode(getSampleMode(uniformBufferLayout, resources->uniformBuffer))
			, uniformOpacity(getOpacity(uniformBufferLayout, resources->uniformBuffer))
			, uniformFlushArea()
			, uniformFlushStages()
		{
			writeDescriptorSets();
			updateModelMatrixUniform(transform);
			updateSampleModeUniform(samplerDesc.samplingMode);
			updateOpacityUniform(opacity);
		}

		~Open() {
			resources->vertexBuffer.waitCompletion(vulkan);
			resources->uniformBuffer.waitCompletion(vulkan);
		}

		void recreate(	ScalingFilter scalingFilter,
						Graphics::RenderPass renderPass,
						BlendingMode blendingMode ) 
		{
			samplerDesc = Graphics::InputColorTransfer::getSamplerDescriptor(scalingFilter);
			pipelineLayout = createPipelineLayout(vulkan, samplerDesc.filter);
			pipeline = Utils::makeShared<vk::UniquePipeline>(createPipeline(vulkan, pipelineLayout, renderPass, blendingMode));

			updateSampleModeUniform(samplerDesc.samplingMode);
		}

		void draw(Graphics::CommandBuffer& cmd, const Video& frame) {
			assert(resources);			
			assert(pipeline);
			assert(frame);

			//Update the vertexbuffer if needed
			resources->vertexBuffer.waitCompletion(vulkan);
			if(geometry.useFrame(*frame)) {
				//Buffer has changed
				resources->vertexBuffer.flushData(
					vulkan,
					vulkan.getGraphicsQueueIndex(),
					vk::AccessFlagBits::eVertexAttributeRead,
					vk::PipelineStageFlagBits::eVertexInput
				);
			}

			//Flush the unform buffer
			resources->uniformBuffer.flushData(
				vulkan,
				uniformFlushArea,
				vulkan.getGraphicsQueueIndex(),
				vk::AccessFlagBits::eUniformRead,
				uniformFlushStages
			);
			uniformFlushArea = {};
			uniformFlushStages = {};

			//Bind the pipeline and its descriptor sets
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

			cmd.bindVertexBuffers(
				VERTEX_BUFFER_BINDING,											//Binding
				resources->vertexBuffer.getBuffer(),							//Vertex buffers
				0UL																//Offsets
			);

			cmd.bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
				pipelineLayout,													//Pipeline layout
				DESCRIPTOR_SET_VIDEOSURFACE,									//First index
				descriptorSet,													//Descriptor sets
				{}																//Dynamic offsets
			);

			frame->bind(
				cmd.getCommandBuffer(), 										//Commandbuffer
				pipelineLayout, 												//Pipeline layout
				DESCRIPTOR_SET_FRAME, 											//Descriptor set index
				samplerDesc.filter												//Filter
			);

			//Draw the frame and finish recording
			cmd.draw(
				Graphics::Frame::Geometry::VERTEX_COUNT, 						//Vertex count
				1, 																//Instance count
				0, 																//First vertex
				0																//First instance
			);

			//Add the dependencies to the command buffer
			cmd.addDependencies({ resources, pipeline, frame });			
		}

		void updateModelMatrixUniform(const Math::Transformf& transform) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);

			uniformModelMatrix = transform.calculateMatrix();
			
			uniformFlushArea |= uniformBufferLayout[DESCRIPTOR_BINDING_MODEL_MATRIX];
			uniformFlushStages |= vk::PipelineStageFlagBits::eVertexShader;
		}

		void updateSampleModeUniform(uint32_t sampleMode) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			uniformSampleMode = sampleMode;
			
			const auto updateArea = Utils::Area(
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_SAMPLEMODE].begin() + uniformBufferLayout[DESCRIPTOR_BINDING_LAYERDATA].begin(),
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_SAMPLEMODE].size()
			);

			uniformFlushArea |= updateArea;
			uniformFlushStages |= vk::PipelineStageFlagBits::eFragmentShader;
		}

		void updateOpacityUniform(float opa) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			uniformOpacity = opa;

			const auto updateArea = Utils::Area(
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_OPACITY].begin() + uniformBufferLayout[DESCRIPTOR_BINDING_LAYERDATA].begin(),
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_OPACITY].size()
			);
			
			uniformFlushArea |= updateArea;
			uniformFlushStages |= vk::PipelineStageFlagBits::eFragmentShader;
		}

	private:
		void writeDescriptorSets() {
			const std::array modelMatrixBuffers = {
				vk::DescriptorBufferInfo(
					resources->uniformBuffer.getBuffer(),							//Buffer
					uniformBufferLayout[DESCRIPTOR_BINDING_MODEL_MATRIX].offset(),	//Offset
					uniformBufferLayout[DESCRIPTOR_BINDING_MODEL_MATRIX].size()		//Size
				)
			};
			const std::array opacityBuffers = {
				vk::DescriptorBufferInfo(
					resources->uniformBuffer.getBuffer(),							//Buffer
					uniformBufferLayout[DESCRIPTOR_BINDING_LAYERDATA].offset(),		//Offset
					uniformBufferLayout[DESCRIPTOR_BINDING_LAYERDATA].size()		//Size
				)
			};

			const std::array writeDescriptorSets = {
				vk::WriteDescriptorSet( //Model matrix UBO
					descriptorSet,											//Descriptor set
					DESCRIPTOR_BINDING_MODEL_MATRIX,						//Binding
					0, 														//Index
					modelMatrixBuffers.size(),								//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					modelMatrixBuffers.data(), 								//Buffers
					nullptr													//Texel buffers
				),
				vk::WriteDescriptorSet( //Layer Data UBO
					descriptorSet,											//Descriptor set
					DESCRIPTOR_BINDING_LAYERDATA,							//Binding
					0, 														//Index
					opacityBuffers.size(),									//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					opacityBuffers.data(), 									//Buffers
					nullptr													//Texel buffers
				)
			};

			vulkan.updateDescriptorSets(writeDescriptorSets, {});
		}



		static Graphics::StagedBuffer createVertexBuffer(const Graphics::Vulkan& vulkan) {
			return Graphics::StagedBuffer(
				vulkan,
				vk::BufferUsageFlagBits::eVertexBuffer,
				sizeof(Vertex) * Graphics::Frame::Geometry::VERTEX_COUNT
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

		static UniformBufferLayout createUniformBufferLayout(const Graphics::Vulkan& vulkan) {
			const auto& limits = vulkan.getPhysicalDeviceProperties().limits;

			constexpr size_t modelMatrixOff = 0;
			constexpr size_t modelMatrixSize = sizeof(glm::mat4);
			
			const size_t layerDataOff = Utils::align(
				modelMatrixOff + modelMatrixSize, 
				limits.minUniformBufferOffsetAlignment
			);
			constexpr size_t layerDataSize = LAYERDATA_UNIFORM_LAYOUT.back().end();

			return UniformBufferLayout {
				Utils::Area(modelMatrixOff,	modelMatrixSize),	//Projection matrix
				Utils::Area(layerDataOff,	layerDataSize )		//Color Transfer
			};
		};

		static Graphics::StagedBuffer createUniformBuffer(const Graphics::Vulkan& vulkan, const UniformBufferLayout& layout) {
			return Graphics::StagedBuffer(
				vulkan,
				vk::BufferUsageFlagBits::eUniformBuffer,
				layout.back().end()
			);
		}

		static vk::UniqueDescriptorPool createDescriptorPool(const Graphics::Vulkan& vulkan){
			const std::array poolSizes = {
				vk::DescriptorPoolSize(
					vk::DescriptorType::eUniformBuffer,					//Descriptor type
					DESCRIPTOR_COUNT									//Descriptor count
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
														vk::Filter filter ) 
		{
			static const std::array<Utils::StaticId, Graphics::Frame::FILTER_COUNT> ids;
			const auto layoutId = ids[static_cast<size_t>(filter)];

			auto result = vulkan.createPipelineLayout(layoutId);

			if(!result) {
				const std::array layouts = {
					RendererBase::getDescriptorSetLayout(vulkan), 			//DESCRIPTOR_SET_RENDERER
					getDescriptorSetLayout(vulkan), 						//DESCRIPTOR_SET_VIDEOSURFACE
					Graphics::Frame::getDescriptorSetLayout(vulkan, filter) //DESCRIPTOR_SET_FRAME
				};

				const vk::PipelineLayoutCreateInfo createInfo(
					{},													//Flags
					layouts.size(), layouts.data(),						//Descriptor set layouts
					0, nullptr											//Push constants
				);

				result = vulkan.createPipelineLayout(layoutId, createInfo);
			}

			return result;
		}

		static vk::UniquePipeline createPipeline(	const Graphics::Vulkan& vulkan,
													vk::PipelineLayout layout,
													Graphics::RenderPass renderPass,
													BlendingMode blendingMode )
		{
			static //So that its ptr can be used as an identifier
			#include <video_surface_vert.h>
			const size_t vertId = reinterpret_cast<uintptr_t>(video_surface_vert);
			static
			#include <video_surface_frag.h>
			const size_t fragId = reinterpret_cast<uintptr_t>(video_surface_frag);

			//Try to retrive modules from cache
			auto vertexShader = vulkan.createShaderModule(vertId);
			if(!vertexShader) {
				//Modules isn't in cache. Create it
				vertexShader = vulkan.createShaderModule(vertId, video_surface_vert);
			}

			auto fragmentShader = vulkan.createShaderModule(fragId);
			if(!fragmentShader) {
				//Modules isn't in cache. Create it
				fragmentShader = vulkan.createShaderModule(fragId, video_surface_frag);
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
					VERTEX_POSITION,
					VERTEX_BUFFER_BINDING,
					vk::Format::eR32G32Sfloat,
					offsetof(Vertex, position)
				),
				vk::VertexInputAttributeDescription(
					VERTEX_TEXCOORD,
					VERTEX_BUFFER_BINDING,
					vk::Format::eR32G32Sfloat,
					offsetof(Vertex, texCoord)
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
				false												//Restart enable
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
				vk::CompareOp::eLess, 								//Depth compare op
				false,												//Depth bounds test
				false, 												//Stencil enabled
				{}, {},												//Stencil operation state front, back
				0.0f, 0.0f											//min, max depth bounds
			);

			const auto colorBlendAttachmentState = Graphics::toVulkan(blendingMode);
			const std::array colorBlendAttachments = {
				colorBlendAttachmentState,
				colorBlendAttachmentState,
				colorBlendAttachmentState,
				colorBlendAttachmentState
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
				renderPass.getRenderPass(), 0,						//Renderpasses
				nullptr, static_cast<uint32_t>(pipelineId)			//Inherit
			);

			return vulkan.createGraphicsPipeline(createInfo);
		}

		static Math::Mat4x4f& getModelMatrix(	const UniformBufferLayout& uniformBufferLayout, 
												Graphics::StagedBuffer& uniformBuffer ) 
		{
			const auto& area = uniformBufferLayout[DESCRIPTOR_BINDING_MODEL_MATRIX];
			return *(reinterpret_cast<Math::Mat4x4f*>(area.begin(uniformBuffer.data())));
		}

		static int32_t& getSampleMode(	const UniformBufferLayout& uniformBufferLayout, 
										Graphics::StagedBuffer& uniformBuffer ) 
		{
			const auto& area1 = uniformBufferLayout[DESCRIPTOR_BINDING_LAYERDATA];
			const auto& area2 = LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_SAMPLEMODE];
			return *(reinterpret_cast<int32_t*>(area1.begin(area2.begin(uniformBuffer.data()))));
		}

		static float& getOpacity(	const UniformBufferLayout& uniformBufferLayout, 
									Graphics::StagedBuffer& uniformBuffer ) 
		{
			const auto& area1 = uniformBufferLayout[DESCRIPTOR_BINDING_LAYERDATA];
			const auto& area2 = LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_OPACITY];
			return *(reinterpret_cast<float*>(area1.begin(area2.begin(uniformBuffer.data()))));
		}

	};

	using Input = Signal::Input<Video>;
	using LastFrames = std::unordered_map<const RendererBase*, Video>;

	std::reference_wrapper<VideoSurface>	owner;

	Input									videoIn;

	Math::Vec2f								size;

	std::unique_ptr<Open>					opened;
	LastFrames								lastFrames;
	

	VideoSurfaceImpl(VideoSurface& owner, Math::Vec2f size)
		: owner(owner)
		, videoIn()
		, size(size)
	{
	}

	~VideoSurfaceImpl() = default;

	void moved(ZuazoBase& base) {
		owner = static_cast<VideoSurface&>(base);
	}

	void open(ZuazoBase& base) {
		assert(!opened);
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface);

		if(videoSurface.getRenderPass() != Graphics::RenderPass()) {
			opened = Utils::makeUnique<Open>(
					videoSurface.getInstance().getVulkan(),
					getSize(),
					videoSurface.getScalingMode(),
					videoSurface.getScalingFilter(),
					videoSurface.getRenderPass(),
					videoSurface.getBlendingMode(),
					videoSurface.getTransform(),
					videoSurface.getOpacity()
			);
		}

		assert(lastFrames.empty()); //Any hasChanged() should return true
	}

	void close(ZuazoBase& base) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);
		
		videoIn.reset();
		opened.reset();
		lastFrames.clear();
	}

	bool hasChangedCallback(const LayerBase& base, const RendererBase& renderer) const {
		const auto& videoSurface = static_cast<const VideoSurface&>(base);
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
		const auto& videoSurface = static_cast<const VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			const auto& frame = videoIn.pull();
			
			//Draw
			if(frame) {
				opened->draw(cmd, frame);
			}

			//Update the state for next hasChanged()
			lastFrames[&renderer] = frame;
		}
	}

	void transformCallback(LayerBase& base, const Math::Transformf& transform) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->updateModelMatrixUniform(transform);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void opacityCallback(LayerBase& base, float opa) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->updateOpacityUniform(opa);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void blendingModeCallback(LayerBase& base, BlendingMode mode) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, videoSurface.getRenderPass(), mode, videoSurface.getScalingFilter());
	}

	void renderPassCallback(LayerBase& base, Graphics::RenderPass renderPass) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, renderPass, videoSurface.getBlendingMode(), videoSurface.getScalingFilter());
	}

	void scalingModeCallback(VideoScalerBase& base, ScalingMode mode) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->geometry.setScalingMode(mode);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void scalingFilterCallback(VideoScalerBase& base, ScalingFilter filter) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, videoSurface.getRenderPass(), videoSurface.getBlendingMode(), filter);
	}


	void setSize(Math::Vec2f s) {
		if(size != s) {
			size = s;

			if(opened) {
				opened->geometry.setTargetSize(s);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

	Math::Vec2f getSize() const {
		return size;
	}

private:
	void recreateCallback(	VideoSurface& videoSurface, 
							Graphics::RenderPass renderPass,
							BlendingMode blendingMode,
							ScalingFilter scalingFilter )
	{
		assert(&owner.get() == &videoSurface);

		if(videoSurface.isOpen()) {
			const bool isValid = 	renderPass != Graphics::RenderPass() &&
									blendingMode > BlendingMode::NONE &&
									scalingFilter > ScalingFilter::NONE ;

			if(opened && isValid) {
				//It remains valid
				opened->recreate(
					scalingFilter,
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
					videoSurface.getInstance().getVulkan(),
					getSize(),
					videoSurface.getScalingMode(),
					scalingFilter,
					renderPass,
					blendingMode,
					videoSurface.getTransform(),
					videoSurface.getOpacity()
				);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

};




VideoSurface::VideoSurface(	Instance& instance,
						std::string name,
						const RendererBase* renderer,
						Math::Vec2f size )
	: Utils::Pimpl<VideoSurfaceImpl>({}, *this, size)
	, ZuazoBase(
		instance, 
		std::move(name),
		{ PadRef((*this)->videoIn) },
		std::bind(&VideoSurfaceImpl::moved, std::ref(**this), std::placeholders::_1),
		std::bind(&VideoSurfaceImpl::open, std::ref(**this), std::placeholders::_1),
		std::bind(&VideoSurfaceImpl::close, std::ref(**this), std::placeholders::_1) )
	, LayerBase(
		renderer,
		std::bind(&VideoSurfaceImpl::transformCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::opacityCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::blendingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::hasChangedCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::drawCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
		std::bind(&VideoSurfaceImpl::renderPassCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, VideoScalerBase(
		std::bind(&VideoSurfaceImpl::scalingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::scalingFilterCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, Signal::ConsumerLayout<Video>(makeProxy((*this)->videoIn))
{
}

VideoSurface::VideoSurface(VideoSurface&& other) = default;

VideoSurface::~VideoSurface() = default;

VideoSurface& VideoSurface::operator=(VideoSurface&& other) = default;


void VideoSurface::setSize(Math::Vec2f size) {
	(*this)->setSize(size);
}

Math::Vec2f VideoSurface::getSize() const {
	return (*this)->getSize();
}

}