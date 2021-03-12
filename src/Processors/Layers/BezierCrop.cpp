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
#include <zuazo/Math/LoopBlinn/OutlineProcessor.h>

#include <utility>
#include <memory>
#include <unordered_map>

namespace Zuazo::Processors::Layers {

struct BezierCropImpl {
	struct Open {
		struct Vertex {
			Vertex(	Math::Vec2f position, 
					Math::Vec2f texCoord = Math::Vec2f(0, 0), 
					Math::Vec3f klm = Math::Vec3f(-1, -1, -1),
					float dist = 0 )
				: position(position)
				, texCoord(texCoord)
				, klm(klm)
				, dist(dist)
			{
			}

			Math::Vec2f position;
			Math::Vec2f texCoord;
			Math::Vec3f klm;
			float		dist;
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
			DESCRIPTOR_SET_BEZIERCROP,
			DESCRIPTOR_SET_FRAME,

			DESCRIPTOR_SET_COUNT
		};

		enum DescriptorBindings {
			DESCRIPTOR_BINDING_MODEL_MATRIX,
			DESCRIPTOR_BINDING_LAYERDATA,

			DESCRIPTOR_COUNT
		};

		enum LayerDataUniforms {
			LAYERDATA_UNIFORM_LINECOLOR,
			LAYERDATA_UNIFORM_LINEWIDTH,
			LAYERDATA_UNIFORM_LINESMOOTHNESS,
			LAYERDATA_UNIFORM_OPACITY,

			LAYERDATA_UNIFORM_COUNT
		};

		static constexpr std::array<Utils::Area, LAYERDATA_UNIFORM_COUNT> LAYERDATA_UNIFORM_LAYOUT = {
			Utils::Area(0,									sizeof(Math::Vec4f) ),	//LAYERDATA_UNIFORM_LINECOLOR
			Utils::Area(sizeof(Math::Vec4f),				sizeof(float)  		),	//LAYERDATA_UNIFORM_LINEWIDTH
			Utils::Area(sizeof(Math::Vec4f)+sizeof(float)*1,sizeof(float)  		),	//LAYERDATA_UNIFORM_LINESMOOTHNESS
			Utils::Area(sizeof(Math::Vec4f)+sizeof(float)*2,sizeof(float)  		)	//LAYERDATA_UNIFORM_OPACITY
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

		Math::LoopBlinn::OutlineProcessor<float, uint16_t>	outlineProcessor;
		Graphics::Frame::Geometry							frameGeometry;

		bool												flushVertexBuffer;
		bool												flushIndexBuffer;

		vk::DescriptorSetLayout								frameDescriptorSetLayout;
		vk::PipelineLayout									pipelineLayout;
		std::shared_ptr<vk::UniquePipeline>					pipeline;

		Open(	const Graphics::Vulkan& vulkan,
				Math::Vec2f size,
				ScalingMode scalingMode,
				Utils::BufferView<const BezierCrop::BezierLoop> crop,
				const Math::Transformf& transform,
				const Math::Vec4f& lineColor,
				float lineWidth,
				float lineSmoothness,
				float opacity ) 
			: vulkan(vulkan)
			, resources(Utils::makeShared<Resources>(	createUniformBuffer(vulkan),
														createDescriptorPool(vulkan) ))
			, descriptorSet(createDescriptorSet(vulkan, *resources->descriptorPool))
			, outlineProcessor()
			, frameGeometry(scalingMode, size)
			, flushVertexBuffer(false)
			, flushIndexBuffer(false)
			, frameDescriptorSetLayout()
			, pipelineLayout()
			, pipeline()
		{
			resources->uniformBuffer.writeDescirptorSet(vulkan, descriptorSet);

			setCrop(crop);
			updateModelMatrixUniform(transform);
			updateLineColorUniform(lineColor);
			updateLineWidthUniform(lineWidth);
			updateLineSmoothnessUniform(lineSmoothness);
			updateOpacityUniform(opacity);
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

			//Update the vertex buffer if needed
			if(frameGeometry.useFrame(*frame)) {
				//Size has changed. Recalculate the vertex buffer
				flushVertexBuffer = true;
			}

			//Upload vertex and index data if necessary
			fillVertexBuffer();
			fillIndexBuffer();

			//Only draw if geometry is defined
			if(resources->indexBuffer.size()) {
				assert(resources->vertexBuffer.size());

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
					DESCRIPTOR_SET_BEZIERCROP,									//First index
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

		void setCrop(Utils::BufferView<const BezierCrop::BezierLoop> crop) {
			outlineProcessor.clear();
			outlineProcessor.addOutline(crop);
			
			flushIndexBuffer = true;
			flushVertexBuffer = true;
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

		void updateLineColorUniform(const Math::Vec4f& color) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			resources->uniformBuffer.write(
				vulkan,
				DESCRIPTOR_BINDING_LAYERDATA,
				&color,
				sizeof(color),
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_LINECOLOR].offset()
			);
		}

		void updateLineWidthUniform(float lineWidth) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			resources->uniformBuffer.write(
				vulkan,
				DESCRIPTOR_BINDING_LAYERDATA,
				&lineWidth,
				sizeof(lineWidth),
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_LINEWIDTH].offset()
			);
		}

		void updateLineSmoothnessUniform(float lineSmooth) {
			assert(resources);
			resources->uniformBuffer.waitCompletion(vulkan);			

			resources->uniformBuffer.write(
				vulkan,
				DESCRIPTOR_BINDING_LAYERDATA,
				&lineSmooth,
				sizeof(lineSmooth),
				LAYERDATA_UNIFORM_LAYOUT[LAYERDATA_UNIFORM_LINESMOOTHNESS].offset()
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

		void fillVertexBuffer() {
			assert(resources);

			if(flushVertexBuffer) {
				const auto surfaceSize = frameGeometry.calculateSurfaceSize();
				const auto& vertices = outlineProcessor.getVertices();

				//Wait for any previous transfers
				resources->vertexBuffer.waitCompletion(vulkan);

				//Recreate if size has changed
				if(resources->vertexBuffer.size() != vertices.size()*sizeof(Vertex)) {
					resources->vertexBuffer = createVertexBuffer(vulkan, vertices.size());
				}

				//Obtain the buffer data
				Utils::BufferView<Vertex> vertexBufferData(
					reinterpret_cast<Vertex*>(resources->vertexBuffer.data()),
					resources->vertexBuffer.size() / sizeof(Vertex)
				);

				//Ensure the size is correct
				assert(vertexBufferData.size() == vertices.size());

				//Copy the data
				for(size_t i = 0; i < vertexBufferData.size(); ++i) {
					//Obtain the interpolation parameter based on the position
					const auto t = Math::ilerp(
						-surfaceSize.first / 2.0f, 
						+surfaceSize.first / 2.0f, 
						vertices[i].pos
					);

					//Interpolate the texture coordinates
					const auto texCoord = Math::lerp(
						(Math::Vec2f(1.0f) - surfaceSize.second) / 2.0f,
						(Math::Vec2f(1.0f) + surfaceSize.second) / 2.0f,
						t
					);

					vertexBufferData[i] = Vertex(
						vertices[i].pos,
						texCoord,
						vertices[i].klm,
						vertices[i].dist
					);
				}

				//Flush the buffer
				resources->vertexBuffer.flushData(
					vulkan, 
					vulkan.getTransferQueueIndex(), 
					vk::AccessFlagBits::eVertexAttributeRead,
					vk::PipelineStageFlagBits::eVertexInput
				);

				flushVertexBuffer = false;
			}

			assert(!flushVertexBuffer);
		}

		void fillIndexBuffer() {
			assert(resources);

			if(flushIndexBuffer) {
				const auto& indices = outlineProcessor.getIndices();

				//Wait for any previous transfers
				resources->indexBuffer.waitCompletion(vulkan);

				//Recreate if size has changed
				if(resources->indexBuffer.size() != indices.size()*sizeof(Index)) {
					resources->indexBuffer = createIndexBuffer(vulkan, indices.size());
				}

				//Ensure the size is correct
				assert(resources->indexBuffer.size() == indices.size()*sizeof(Index));

				//Copy the data
				std::memcpy(
					resources->indexBuffer.data(), 
					indices.data(), 
					indices.size()*sizeof(Index)
				);

				//Flush the buffer
				resources->indexBuffer.flushData(
					vulkan, 
					vulkan.getTransferQueueIndex(), 
					vk::AccessFlagBits::eIndexRead,
					vk::PipelineStageFlagBits::eVertexInput
				);

				flushIndexBuffer = false;
			}

			assert(!flushIndexBuffer);
		}



		static Graphics::StagedBuffer createVertexBuffer(const Graphics::Vulkan& vulkan, size_t vertexCount) {
			if(vertexCount > 0) {
				return Graphics::StagedBuffer(
					vulkan,
					vk::BufferUsageFlagBits::eVertexBuffer,
					sizeof(Vertex) * vertexCount
				);
			} else {
				return {};
			}
		}

		static Graphics::StagedBuffer createIndexBuffer(const Graphics::Vulkan& vulkan, size_t indexCount) {
			if(indexCount > 0) {
				return Graphics::StagedBuffer(
					vulkan,
					vk::BufferUsageFlagBits::eIndexBuffer,
					sizeof(Index) * indexCount
				);
			} else {
				return {};
			}
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
					getDescriptorSetLayout(vulkan), 						//DESCRIPTOR_SET_BEZIERCROP
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
					vk::Format::eR32G32B32Sfloat,
					offsetof(Vertex, klm)
				),
				vk::VertexInputAttributeDescription(
					3, //TODO
					VERTEX_BUFFER_BINDING,
					vk::Format::eR32Sfloat,
					offsetof(Vertex, dist)
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
				vk::CullModeFlagBits::eBack, 						//Cull faces
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

	Math::Vec2f								size;
	std::vector<BezierCrop::BezierLoop>		crop;
	Math::Vec4f								lineColor;
	float									lineWidth;
	float									lineSmoothness;

	std::unique_ptr<Open>					opened;
	LastFrames								lastFrames;
	

	BezierCropImpl(	BezierCrop& owner, 
					Math::Vec2f size, 
					Utils::BufferView<const BezierCrop::BezierLoop> crop )
		: owner(owner)
		, videoIn()
		, size(size)
		, crop(crop.cbegin(), crop.cend())
		, lineColor(0)
		, lineWidth(0)
		, lineSmoothness(1)
	{
	}

	~BezierCropImpl() = default;

	void moved(ZuazoBase& base) {
		owner = static_cast<BezierCrop&>(base);
	}

	void open(ZuazoBase& base, std::unique_lock<Instance>* lock = nullptr) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop);
		assert(!opened);

		if(bezierCrop.getRenderPass() != Graphics::RenderPass()) {
			//Create in a unlocked environment
			if(lock) lock->unlock();
			auto newOpened = Utils::makeUnique<Open>(
					bezierCrop.getInstance().getVulkan(),
					getSize(),
					bezierCrop.getScalingMode(),
					getCrop(),
					bezierCrop.getTransform(),
					bezierCrop.getLineColor(),
					bezierCrop.getLineWidth(),
					bezierCrop.getLineSmoothness(),
					bezierCrop.getOpacity()
			);
			if(lock) lock->lock();

			//Write changes after locking back
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
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

		//Write changes
		videoIn.reset();
		lastFrames.clear();
		auto oldOpened = std::move(opened);

		//Destroy the object in a unlocked environment
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
		const auto& bezierCrop = static_cast<const BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

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
		const auto& bezierCrop = static_cast<const BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

		if(opened) {
			const auto& frame = videoIn.pull();
			
			//Draw
			if(frame) {
				opened->draw(
					cmd, 
					frame, 
					bezierCrop.getScalingFilter(),
					bezierCrop.getRenderPass(),
					bezierCrop.getBlendingMode()
				);
			}

			//Update the state for next hasChanged()
			lastFrames[&renderer] = frame;
		}
	}

	void transformCallback(LayerBase& base, const Math::Transformf& transform) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

		if(opened) {
			opened->updateModelMatrixUniform(transform);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void opacityCallback(LayerBase& base, float opa) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

		if(opened) {
			opened->updateOpacityUniform(opa);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void blendingModeCallback(LayerBase& base, BlendingMode mode) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		recreateCallback(bezierCrop, bezierCrop.getRenderPass(), mode);
	}

	void renderPassCallback(LayerBase& base, Graphics::RenderPass renderPass) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		recreateCallback(bezierCrop, renderPass, bezierCrop.getBlendingMode());
	}

	void scalingModeCallback(VideoScalerBase& base, ScalingMode mode) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

		if(opened) {
			opened->frameGeometry.setScalingMode(mode);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	void scalingFilterCallback(VideoScalerBase& base, ScalingFilter) {
		auto& bezierCrop = static_cast<BezierCrop&>(base);
		assert(&owner.get() == &bezierCrop); (void)(bezierCrop);

		lastFrames.clear(); //Will force hasChanged() to true
	}


	void setSize(Math::Vec2f size) {
		if(this->size != size) {
			this->size = size;

			if(opened) {
				opened->frameGeometry.setTargetSize(this->size);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

	Math::Vec2f getSize() const {
		return size;
	}


	void setCrop(Utils::BufferView<const BezierCrop::BezierLoop> crop) {
		this->crop.insert(this->crop.cend(), crop.cbegin(), crop.cend());

		if(opened) {
			opened->setCrop(this->crop);
		}

		lastFrames.clear(); //Will force hasChanged() to true
	}

	Utils::BufferView<const BezierCrop::BezierLoop> getCrop() const {
		return crop;
	}

	void setLineColor(const Math::Vec4f& color) {
		if(this->lineColor != color) {
			this->lineColor = color;

			if(opened) {
				opened->updateLineColorUniform(this->lineColor);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

	const Math::Vec4f& getLineColor() const {
		return lineColor;
	}


	void setLineWidth(float lineWidth) {
		if(this->lineWidth != lineWidth) {
			this->lineWidth = lineWidth;

			if(opened) {
				opened->updateLineWidthUniform(this->lineWidth);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

	float getLineWidth() const {
		return lineWidth;
	}


	void setLineSmoothness(float lineSmoothness) {
		if(this->lineSmoothness != lineSmoothness) {
			this->lineSmoothness = lineSmoothness;

			if(opened) {
				opened->updateLineSmoothnessUniform(this->lineSmoothness);
			}

			lastFrames.clear(); //Will force hasChanged() to true
		}
	}

	float getLineSmoothness() const {
		return lineSmoothness;
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
					getSize(),
					bezierCrop.getScalingMode(),
					getCrop(),
					bezierCrop.getTransform(),
					bezierCrop.getLineColor(),
					bezierCrop.getLineWidth(),
					bezierCrop.getLineSmoothness(),
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
						Math::Vec2f size,
						Utils::BufferView<const BezierLoop> crop )
	: Utils::Pimpl<BezierCropImpl>({}, *this, size, crop)
	, ZuazoBase(
		instance, 
		std::move(name),
		{ PadRef((*this)->videoIn) },
		std::bind(&BezierCropImpl::moved, std::ref(**this), std::placeholders::_1),
		std::bind(&BezierCropImpl::open, std::ref(**this), std::placeholders::_1, nullptr),
		std::bind(&BezierCropImpl::asyncOpen, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&BezierCropImpl::close, std::ref(**this), std::placeholders::_1, nullptr),
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


void BezierCrop::setSize(Math::Vec2f size) {
	(*this)->setSize(size);
}

Math::Vec2f BezierCrop::getSize() const {
	return (*this)->getSize();
}


void BezierCrop::setCrop(Utils::BufferView<const BezierLoop> crop) {
	(*this)->setCrop(crop);
}

Utils::BufferView<const BezierCrop::BezierLoop> BezierCrop::getCrop() const {
	return (*this)->getCrop();
}


void BezierCrop::setLineColor(const Math::Vec4f& color) {
	(*this)->setLineColor(color);
}

const Math::Vec4f& BezierCrop::getLineColor() const {
	return (*this)->getLineColor();
}


void BezierCrop::setLineWidth(float width) {
	(*this)->setLineWidth(width);
}

float BezierCrop::getLineWidth() const {
	return (*this)->getLineWidth();
}


void BezierCrop::setLineSmoothness(float smoothness) {
	(*this)->setLineSmoothness(smoothness);
}

float BezierCrop::getLineSmoothness() const {
	return (*this)->getLineSmoothness();
}

}