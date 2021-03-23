#include <zuazo/Processors/Layers/VideoSurface.h>

#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/StaticId.h>
#include <zuazo/Utils/Hasher.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Graphics/StagedBuffer.h>
#include <zuazo/Graphics/UniformBuffer.h>
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
			VERTEX_LOCATION_POSITION,
			VERTEX_LOCATION_TEXCOORD,

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
			Resources(	Graphics::StagedBuffer vertexBuffer,
						Graphics::UniformBuffer uniformBuffer,
						vk::UniqueDescriptorPool descriptorPool )
				: vertexBuffer(std::move(vertexBuffer))
				, uniformBuffer(std::move(uniformBuffer))
				, descriptorPool(std::move(descriptorPool))
			{
			}

			~Resources() = default;

			Graphics::StagedBuffer								vertexBuffer;
			Graphics::UniformBuffer								uniformBuffer;
			vk::UniqueDescriptorPool							descriptorPool;
		};

		const Graphics::Vulkan&								vulkan;

		std::shared_ptr<Resources>							resources;
		Graphics::Frame::Geometry							geometry;
		vk::DescriptorSet									descriptorSet;

		vk::DescriptorSetLayout								frameDescriptorSetLayout;
		vk::PipelineLayout									pipelineLayout;
		vk::Pipeline										pipeline;

		Open(	const Graphics::Vulkan& vulkan,
				Math::Vec2f size,
				ScalingMode scalingMode,
				const Math::Transformf& transform,
				float opacity ) 
			: vulkan(vulkan)
			, resources(Utils::makeShared<Resources>(	createVertexBuffer(vulkan),
														createUniformBuffer(vulkan),
														createDescriptorPool(vulkan) ))
			, geometry(scalingMode, size)
			, descriptorSet(createDescriptorSet(vulkan, *resources->descriptorPool))
			, frameDescriptorSetLayout()
			, pipelineLayout()
			, pipeline()
		{
			resources->uniformBuffer.writeDescirptorSet(vulkan, descriptorSet);
			updateModelMatrixUniform(transform);
			updateOpacityUniform(opacity);
		}

		~Open() {
			resources->vertexBuffer.waitCompletion(vulkan);
			resources->uniformBuffer.waitCompletion(vulkan);
		}

		void recreate() 
		{
			//This will enforce recreation when the next frame is rendered
			frameDescriptorSetLayout = nullptr;
		}

		void draw(	Graphics::CommandBuffer& cmd, 
					const Video& frame, 
					ScalingFilter filter,
					Graphics::RenderPass renderPass,
					BlendingMode blendingMode,
					RenderingLayer renderingLayer ) 
		{				
			assert(resources);			
			assert(frame);

			//Update the vertex buffer if needed
			if(geometry.useFrame(*frame)) {
				//Size has changed
				resources->vertexBuffer.waitCompletion(vulkan);

				//Write the new data
				geometry.writeQuadVertices(
					reinterpret_cast<Math::Vec2f*>(resources->vertexBuffer.data() + offsetof(Vertex, position)),
					reinterpret_cast<Math::Vec2f*>(resources->vertexBuffer.data() + offsetof(Vertex, texCoord)),
					sizeof(Vertex),
					sizeof(Vertex)
				);

				//Flush the buffer
				resources->vertexBuffer.flushData(
					vulkan,
					vulkan.getGraphicsQueueIndex(),
					vk::AccessFlagBits::eVertexAttributeRead,
					vk::PipelineStageFlagBits::eVertexInput
				);
			}

			//Flush the unform buffer
			resources->uniformBuffer.flush(vulkan);

			//Configure the sampler for propper operation
			configureSampler(*frame, filter, renderPass, blendingMode, renderingLayer);
			assert(frameDescriptorSetLayout);
			assert(pipelineLayout);
			assert(pipeline);

			//Bind the pipeline and its descriptor sets
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

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
				cmd.get(), 														//Commandbuffer
				pipelineLayout, 												//Pipeline layout
				DESCRIPTOR_SET_FRAME, 											//Descriptor set index
				filter															//Filter
			);

			//Draw the frame and finish recording
			cmd.draw(
				Graphics::Frame::Geometry::VERTEX_COUNT, 						//Vertex count
				1, 																//Instance count
				0, 																//First vertex
				0																//First instance
			);

			//Add the dependencies to the command buffer
			cmd.addDependencies({ resources, frame });			
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
								BlendingMode blendingMode,
								RenderingLayer renderingLayer ) 
		{
			const auto newDescriptorSetLayout = frame.getDescriptorSetLayout(filter);
			if(frameDescriptorSetLayout != newDescriptorSetLayout) {
				frameDescriptorSetLayout = newDescriptorSetLayout;

				//Recreate stuff
				pipelineLayout = createPipelineLayout(vulkan, frameDescriptorSetLayout);
				pipeline = createPipeline(vulkan, pipelineLayout, renderPass, blendingMode, renderingLayer);
			}
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

		static Utils::BufferView<const std::pair<uint32_t, size_t>> getUniformBufferSizes() noexcept {
			static const std::array uniformBufferSizes = {
				std::make_pair<uint32_t, size_t>(DESCRIPTOR_BINDING_MODEL_MATRIX, 	sizeof(Math::Mat4x4f) ),
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

		static vk::Pipeline createPipeline(	const Graphics::Vulkan& vulkan,
													vk::PipelineLayout layout,
													Graphics::RenderPass renderPass,
													BlendingMode blendingMode,
													RenderingLayer renderingLayer )
		{
			using Index = std::tuple<	vk::PipelineLayout,
										vk::RenderPass,
										BlendingMode,
										RenderingLayer >;
			static std::unordered_map<Index, const Utils::StaticId, Utils::Hasher<Index>> ids;

			//Obtain the id related to the configuration
			Index index(layout, renderPass.get(), blendingMode, renderingLayer);
			const auto& id = ids[index];

			//Try to obtain it from cache
			auto result = vulkan.createGraphicsPipeline(id);
			if(!result) {
				//No luck, we need to create it
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

				const auto depthStencil = Graphics::getDepthStencilConfiguration(renderingLayer);

				const std::array colorBlendAttachments = {
					Graphics::getBlendingConfiguration(blendingMode)
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
					nullptr, 0											//Inherit
				);

				result = vulkan.createGraphicsPipeline(id, createInfo);
			}

			assert(result);
			return result;
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

	void open(ZuazoBase& base, std::unique_lock<Instance>* lock = nullptr) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface);
		assert(!opened);

		if(videoSurface.getRenderPass() != Graphics::RenderPass()) {
			//Create in a unlocked environment
			if(lock) lock->unlock();
			auto newOpened = Utils::makeUnique<Open>(
					videoSurface.getInstance().getVulkan(),
					getSize(),
					videoSurface.getScalingMode(),
					videoSurface.getTransform(),
					videoSurface.getOpacity()
			);
			if(lock) lock->lock();

			//Write changes after locking
			opened = std::move(newOpened);
		}

		assert(lastFrames.empty()); //Any hasChanged() should return true
	}

	void asyncOpen(ZuazoBase& base, std::unique_lock<Instance>& lock) {
		assert(lock.owns_lock());
		open(base, &lock);
		assert(lock.owns_lock());
	}


	void close(ZuazoBase& base, std::unique_lock<Instance>* lock = nullptr) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);
		
		//Write changes
		videoIn.reset();
		lastFrames.clear();
		auto oldOpened = std::move(opened);

		//Reset in a unlocked environment
		if(oldOpened) {
			if(lock) lock->unlock();
			oldOpened.reset();
			if(lock) lock->lock();
		}

		assert(!opened);
	}

	void asyncClose(ZuazoBase& base, std::unique_lock<Instance>& lock) {
		assert(lock.owns_lock());
		close(base, &lock);
		assert(lock.owns_lock());
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
				opened->draw(
					cmd, 
					frame, 
					videoSurface.getScalingFilter(),
					videoSurface.getRenderPass(),
					videoSurface.getBlendingMode(),
					videoSurface.getRenderingLayer()
				);
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
		recreateCallback(videoSurface, videoSurface.getRenderPass(), mode);
	}

	void renderingLayerCallback(LayerBase& base, RenderingLayer) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, videoSurface.getRenderPass(), videoSurface.getBlendingMode());
	}

	void renderPassCallback(LayerBase& base, Graphics::RenderPass renderPass) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		recreateCallback(videoSurface, renderPass, videoSurface.getBlendingMode());
	}

	void scalingModeCallback(VideoScalerBase& base, ScalingMode mode) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		if(opened) {
			opened->geometry.setScalingMode(mode);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void scalingFilterCallback(VideoScalerBase& base, ScalingFilter) {
		auto& videoSurface = static_cast<VideoSurface&>(base);
		assert(&owner.get() == &videoSurface); (void)(videoSurface);

		lastFrames.clear(); //Will force hasChanged() to true
	}


	void setSize(Math::Vec2f size) {
		if(this->size != size) {
			this->size = size;

			if(opened) {
				opened->geometry.setTargetSize(this->size);
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
							BlendingMode blendingMode )
	{
		assert(&owner.get() == &videoSurface);

		if(videoSurface.isOpen()) {
			const bool isValid = 	renderPass != Graphics::RenderPass() &&
									blendingMode > BlendingMode::NONE ;

			if(opened && isValid) {
				//It remains valid
				opened->recreate();
			} else if(opened && !isValid) {
				//It has become invalid
				videoIn.reset();
				opened.reset();
			} else if(!opened && isValid) {
				//It has become valid
				open(videoSurface, nullptr);
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
		std::bind(&VideoSurfaceImpl::open, std::ref(**this), std::placeholders::_1, nullptr),
		std::bind(&VideoSurfaceImpl::asyncOpen, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::close, std::ref(**this), std::placeholders::_1, nullptr),
		std::bind(&VideoSurfaceImpl::asyncClose, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, LayerBase(
		renderer,
		std::bind(&VideoSurfaceImpl::transformCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::opacityCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::blendingModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&VideoSurfaceImpl::renderingLayerCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
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