#include <zuazo/Processors/Layers/VideoSurface.h>

#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/StaticId.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Graphics/StagedBuffer.h>
#include <zuazo/Graphics/CommandBufferPool.h>

#include <utility>
#include <memory>

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
			DESCRIPTOR_BINDING_OPACITY,

			DESCRIPTOR_COUNT
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
		vk::DescriptorSet									descriptorSet;

		vk::Filter											filter;
		vk::PipelineLayout									pipelineLayout;
		std::shared_ptr<vk::UniquePipeline>					pipeline;

		Graphics::CommandBufferPool							commandBufferPool;
		Utils::Pool<LayerData>								layerDataPool;

		Math::Mat4x4f&										uniformModelMatrix;
		float&												uniformOpacity;
		Utils::Area											uniformFlushArea;
		vk::PipelineStageFlags								uniformFlushStages;

		Open(	const Graphics::Vulkan& vulkan,
				Math::Vec2f size,
				ScalingMode scalingMode,
				ScalingFilter scalingFilter,
				vk::RenderPass renderPass,
				uint32_t attachmentCount,
				BlendingMode blendingMode,
				const Math::Transformf& transform,
				float opacity ) 
			: vulkan(vulkan)
			, uniformBufferLayout(createUniformBufferLayout(vulkan))
			, resources(Utils::makeShared<Resources>(	createVertexBuffer(vulkan),
														createUniformBuffer(vulkan, uniformBufferLayout),
														createDescriptorPool(vulkan) ))
			, geometry(resources->vertexBuffer.data(), sizeof(Vertex), offsetof(Vertex, position), offsetof(Vertex, texCoord), scalingMode, size)
			, descriptorSet(createDescriptorSet(vulkan, *resources->descriptorPool))
			, filter(Graphics::toVulkan(scalingFilter))
			, pipelineLayout(createPipelineLayout(vulkan, filter))
			, pipeline(Utils::makeShared<vk::UniquePipeline>(createPipeline(vulkan, pipelineLayout, renderPass, attachmentCount, blendingMode)))
			, commandBufferPool(createCommandBufferPool(vulkan))
			, layerDataPool()
			, uniformModelMatrix(getModelMatrix(uniformBufferLayout, resources->uniformBuffer))
			, uniformOpacity(getOpacity(uniformBufferLayout, resources->uniformBuffer))
			, uniformFlushArea()
			, uniformFlushStages()
		{
			updateModelMatrixUniform(transform);
			updateOpacityUniform(opacity);
		}

		~Open() = default;

		void recreate(	ScalingFilter scalingFilter,
						vk::RenderPass renderPass,
						uint32_t attachmentCount,
						BlendingMode blendingMode ) 
		{
			filter = Graphics::toVulkan(scalingFilter);
			pipelineLayout = createPipelineLayout(vulkan, filter);
			pipeline = Utils::makeShared<vk::UniquePipeline>(createPipeline(vulkan, pipelineLayout, renderPass, attachmentCount, blendingMode));
		}

		LayerDataStream process(const Video& frame) {
			auto commandBuffer = commandBufferPool.acquireCommandBuffer();
			assert(commandBuffer);
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

			//Begin the command buffer
			constexpr vk::CommandBufferUsageFlags beginFlags =
				vk::CommandBufferUsageFlagBits::eOneTimeSubmit | 
				vk::CommandBufferUsageFlagBits::eRenderPassContinue ;
			constexpr vk::CommandBufferBeginInfo beginInfo(beginFlags, nullptr);
			commandBuffer->begin(beginInfo);

			//Bind the pipeline and its descriptor sets
			commandBuffer->bindPipeline(vk::PipelineBindPoint::eGraphics, **pipeline);

			commandBuffer->bindVertexBuffers(
				VERTEX_BUFFER_BINDING,											//Binding
				resources->vertexBuffer.getBuffer(),							//Vertex buffers
				0UL																//Offsets
			);

			commandBuffer->bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
				pipelineLayout,													//Pipeline layout
				DESCRIPTOR_SET_VIDEOSURFACE,									//First index
				descriptorSet,													//Descriptor sets
				{}																//Dynamic offsets
			);

			frame->bind(
				commandBuffer->getCommandBuffer(), 								//Commandbuffer
				pipelineLayout, 												//Pipeline layout
				DESCRIPTOR_SET_FRAME, 											//Descriptor set index
				filter															//Filter
			);

			//Draw the frame and finish recording
			commandBuffer->draw(
				Graphics::Frame::Geometry::VERTEX_COUNT, 						//Vertex count
				1, 																//Instance count
				0, 																//First vertex
				0																//First instance
			);
			commandBuffer->end();

			//Add the dependencies to the command buffer
			const std::array<Graphics::CommandBuffer::Dependency, 3> dependencies = {
				resources,
				pipeline,
				frame
			};
			commandBuffer->setDependencies(dependencies);
			

			//Elaborate the result
			auto result = layerDataPool.acquire();
			assert(result);

			result->setCommandBuffer(std::move(commandBuffer));
			result->setAveragePosition(uniformModelMatrix[3]); //uniformModelMatrix * Math::Vec4f(0.0f, 0.0f, 0.0f, 1.0f)
			result->setHasAlpha(uniformOpacity != 1.0f || hasAlpha(frame->getDescriptor().getColorFormat()));

			assert(result->getAveragePosition() == uniformModelMatrix * Math::Vec4f(0.0f, 0.0f, 0.0f, 1.0f));
			return result;
		}

		void updateModelMatrixUniform(const Math::Transformf& transform) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);

			uniformModelMatrix = transform.calculateMatrix();
			
			uniformFlushArea |= uniformBufferLayout[DESCRIPTOR_BINDING_MODEL_MATRIX];
			uniformFlushStages |= vk::PipelineStageFlagBits::eVertexShader;
		}

		void updateOpacityUniform(float opa) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			uniformOpacity = opa;
			
			uniformFlushArea |= uniformBufferLayout[DESCRIPTOR_BINDING_OPACITY];
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
					uniformBufferLayout[DESCRIPTOR_BINDING_OPACITY].offset(),		//Offset
					uniformBufferLayout[DESCRIPTOR_BINDING_OPACITY].size()			//Size
				)
			};

			const std::array writeDescriptorSets = {
				vk::WriteDescriptorSet( //Viewport UBO
					descriptorSet,											//Descriptor set
					DESCRIPTOR_BINDING_MODEL_MATRIX,						//Binding
					0, 														//Index
					modelMatrixBuffers.size(),								//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					modelMatrixBuffers.data(), 								//Buffers
					nullptr													//Texel buffers
				),
				vk::WriteDescriptorSet( //ColorTransfer UBO
					descriptorSet,											//Descriptor set
					DESCRIPTOR_BINDING_OPACITY,								//Binding
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
						DESCRIPTOR_BINDING_OPACITY,						//Binding
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
			
			const size_t opacityOff = Utils::align(
				modelMatrixOff + modelMatrixSize, 
				limits.minUniformBufferOffsetAlignment
			);
			constexpr size_t opacitySize = sizeof(float);

			return UniformBufferLayout {
				Utils::Area(modelMatrixOff,	modelMatrixSize),	//Projection matrix
				Utils::Area(opacityOff,		opacitySize )		//Color Transfer
			};
		}

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
													vk::RenderPass renderPass,
													uint32_t colorAttachmentCount,
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
			auto fragmentShader = vulkan.createShaderModule(fragId);

			if(!vertexShader || !fragmentShader) {
				//Modules aren't in cache. Create them
				vertexShader = vulkan.createShaderModule(vertId, video_surface_vert);
				fragmentShader = vulkan.createShaderModule(fragId, video_surface_frag);
			}

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

			const std::vector<vk::PipelineColorBlendAttachmentState> colorBlendAttachments(
				colorAttachmentCount,
				Graphics::toVulkan(blendingMode)
			);

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
				renderPass, 0,										//Renderpasses
				nullptr, static_cast<uint32_t>(pipelineId)			//Inherit
			);

			return vulkan.createGraphicsPipeline(createInfo);
		}

		static Graphics::CommandBufferPool createCommandBufferPool(const Graphics::Vulkan& vulkan) {
			constexpr vk::CommandPoolCreateFlags createFlags = 
				vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
				vk::CommandPoolCreateFlagBits::eTransient ;

			return Graphics::CommandBufferPool(
				vulkan,
				createFlags,
				vulkan.getGraphicsQueueIndex(),
				vk::CommandBufferLevel::eSecondary
			);
		}

		static Math::Mat4x4f& getModelMatrix(	const UniformBufferLayout& uniformBufferLayout, 
												Graphics::StagedBuffer& uniformBuffer ) 
		{
			const auto& area = uniformBufferLayout[DESCRIPTOR_BINDING_MODEL_MATRIX];
			return *(reinterpret_cast<Math::Mat4x4f*>(area.begin(uniformBuffer.data())));
		}

		static float& getOpacity(	const UniformBufferLayout& uniformBufferLayout, 
									Graphics::StagedBuffer& uniformBuffer ) 
		{
			const auto& area = uniformBufferLayout[DESCRIPTOR_BINDING_OPACITY];
			return *(reinterpret_cast<float*>(area.begin(uniformBuffer.data())));
		}

	};

	using Input = Signal::Input<Video>;
	using Output = Signal::Output<LayerDataStream>;

	std::reference_wrapper<VideoSurface>	owner;

	Input									videoIn;
	Output									layerOut;

	Math::Vec2f								size;

	std::unique_ptr<Open>					opened;
	bool									hasChanged;

	VideoSurfaceImpl(VideoSurface& owner, Math::Vec2f size)
		: owner(owner)
		, videoIn()
		, layerOut(std::string(Signal::makeOutputName<LayerDataStream>()), createPullCallback(this))
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

		const auto renderPass = videoSurface.getRenderPass();
		if(renderPass) {
			opened = Utils::makeUnique<Open>(
					videoSurface.getInstance().getVulkan(),
					getSize(),
					videoSurface.getScalingMode(),
					videoSurface.getScalingFilter(),
					videoSurface.getRenderPass(),
					videoSurface.getColorAttachmentCount(), 
					videoSurface.getBlendingMode(),
					videoSurface.getTransform(),
					videoSurface.getOpacity()
			);
		}

		hasChanged = true; //Signal rendering if needed
	}

	void close(ZuazoBase& base) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface);
		
		videoIn.reset();
		layerOut.reset();
		opened.reset();
	}

	void update() {
		if(opened) {
			if(hasChanged || videoIn.hasChanged()) {
				const auto& frame = videoIn.pull();
				layerOut.push(frame ? opened->process(frame) : LayerDataStream());

				//Update the state
				hasChanged = false;
			}
		}
	}

	void transformCallback(LayerBase& base, const Math::Transformf& transform) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->updateModelMatrixUniform(transform);
		}

		hasChanged = true;
	}

	void opacityCallback(LayerBase& base, float opa) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->updateOpacityUniform(opa);
		}

		hasChanged = true;
	}

	void blendingModeCallback(LayerBase& base, BlendingMode mode) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, videoSurface.getRenderPass(), videoSurface.getColorAttachmentCount(), mode, videoSurface.getScalingFilter());
	}

	void renderPassCallback(LayerBase& base, vk::RenderPass renderPass, uint32_t attachmentCount) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, renderPass, attachmentCount, videoSurface.getBlendingMode(), videoSurface.getScalingFilter());
	}

	void scalingModeCallback(VideoScalerBase& base, ScalingMode mode) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->geometry.setScalingMode(mode);
		}

		hasChanged = true;
	}

	void scalingFilterCallback(VideoScalerBase& base, ScalingFilter filter) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, videoSurface.getRenderPass(), videoSurface.getColorAttachmentCount(), videoSurface.getBlendingMode(), filter);
	}


	void setSize(Math::Vec2f s) {
		if(size != s) {
			size = s;

			if(opened) {
				opened->geometry.setTargetSize(s);
			}

			hasChanged = true;
		}
	}

	Math::Vec2f getSize() const {
		return size;
	}

private:
	void recreateCallback(	VideoSurface& videoSurface, 
							vk::RenderPass renderPass,
							uint32_t attachmentCount,
							BlendingMode blendingMode,
							ScalingFilter scalingFilter )
	{
		assert(&owner.get() == &videoSurface);

		if(videoSurface.isOpen()) {
			const bool isValid = 	renderPass != vk::RenderPass() &&
									blendingMode > BlendingMode::NONE &&
									scalingFilter > ScalingFilter::NONE ;

			if(opened && isValid) {
				//It remains valid
				opened->recreate(
					scalingFilter,
					renderPass,
					attachmentCount,
					blendingMode
				);
			} else if(opened && !isValid) {
				//It has become invalid
				videoIn.reset();
				layerOut.reset();
				opened.reset();
			} else if(!opened && isValid) {
				//It has become valid
				opened = Utils::makeUnique<Open>(
					videoSurface.getInstance().getVulkan(),
					getSize(),
					videoSurface.getScalingMode(),
					scalingFilter,
					renderPass,
					attachmentCount, 
					blendingMode,
					videoSurface.getTransform(),
					videoSurface.getOpacity()
				);
			}

			hasChanged = true; //Signal rendering if needed
		}
	}

	static Output::PullCallback createPullCallback(VideoSurfaceImpl* impl) {
		return [impl] (Output&) {
			impl->owner.get().update();
		};
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
		{ PadRef((*this)->videoIn), PadRef((*this)->layerOut) },
		std::bind(&VideoSurfaceImpl::moved, std::ref(**this), std::placeholders::_1),
		std::bind(&VideoSurfaceImpl::open, std::ref(**this), std::placeholders::_1),
		std::bind(&VideoSurfaceImpl::close, std::ref(**this), std::placeholders::_1),
		std::bind(&VideoSurfaceImpl::update, std::ref(**this)) )
	, LayerBase(
		renderer,
		std::bind(&VideoSurfaceImpl::transformCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::opacityCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::blendingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::renderPassCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3) )
	, VideoScalerBase(
		std::bind(&VideoSurfaceImpl::scalingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::scalingFilterCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, Signal::ProcessorLayout<Video, LayerDataStream>(makeProxy((*this)->videoIn), makeProxy((*this)->layerOut))
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