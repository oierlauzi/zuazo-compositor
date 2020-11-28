#include <zuazo/Processors/Compositor.h>

#include <zuazo/LayerData.h>
#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Graphics/StagedBuffer.h>
#include <zuazo/Graphics/Drawtable.h>
#include <zuazo/Graphics/CommandBufferPool.h>
#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Utils/StaticId.h>


#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <tuple>
#include <bitset>

namespace Zuazo::Processors {

/*
 * CompositorImpl
 */

struct CompositorImpl {
	struct Open {
		enum DescriptorLayouts {
			DESCRIPTOR_LAYOUT_COMPOSITOR,

			DESCRIPTOR_LAYOUT_COUNT
		};

		enum CompositorDescriptors {
			COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX,
			COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER,

			COMPOSITOR_DESCRIPTOR_COUNT
		};

		using UniformBufferLayout = std::array<Utils::Area, COMPOSITOR_DESCRIPTOR_COUNT>;

		using CommandPool = Utils::Pool<Graphics::CommandBuffer>;
		using Layers = std::vector<std::reference_wrapper<const LayerData>>;

		struct Cache {
			std::vector<vk::CommandBuffer>				drawCommandBuffers;
			std::vector<std::shared_ptr<const void>>	dependencies;
		};

		const Graphics::Vulkan& 					vulkan;

		UniformBufferLayout							uniformBufferLayout;
		Graphics::StagedBuffer						uniformBuffer;
		vk::UniqueDescriptorPool					descriptorPool;
		vk::DescriptorSet							descriptorSet;
		vk::PipelineLayout							pipelineLayout;

		Graphics::Drawtable 						drawtable;
		Graphics::OutputColorTransfer				colorTransfer;
		Graphics::CommandBufferPool					commandBufferPool;
		
		std::vector<vk::ClearValue>					clearValues;

		std::shared_ptr<const Graphics::Frame>		lastFrame;

		Layers										layers;
		Cache										cache;

		Open(	const Graphics::Vulkan& vulkan, 
				const Graphics::Frame::Descriptor& desc,
				DepthStencilFormat depthStencilFmt,
				const Compositor::Camera& cam )
			: vulkan(vulkan)
			, uniformBufferLayout(createUniformBufferLayout(vulkan))
			, uniformBuffer(createUniformBuffer(vulkan, uniformBufferLayout))
			, descriptorPool(createDescriptorPool(vulkan))
			, descriptorSet(createDescriptorSet(vulkan, *descriptorPool))
			, pipelineLayout(createPipelineLayout(vulkan))

			, drawtable(createDrawtable(vulkan, desc, depthStencilFmt))
			, colorTransfer(drawtable.getOutputColorTransfer())
			, commandBufferPool(createCommandBufferPool(vulkan))

			, clearValues(createClearValues(desc, depthStencilFmt))
		{
			//Bind the uniform buffers to the descriptor sets
			writeDescriptorSets();

			//Update the contents of the uniforms buffers
			updateUniforms(cam);
		}

		Open(const Open& other) = delete;

		~Open() {
			uniformBuffer.waitCompletion(vulkan);
		}

		void recreate(	const Graphics::Frame::Descriptor& desc,
						DepthStencilFormat depthStencilFmt,
						const Compositor::Camera& cam )
		{
			enum {
				RECREATE_DRAWTABLE,
				RECREATE_CLEAR_VALUES,
				UPDATE_PROJECTION_MATRIX,
				UPDATE_COLOR_TRANSFER,

				MODIFICATION_COUNT
			};

			std::bitset<MODIFICATION_COUNT> modifications;

			modifications.set(RECREATE_DRAWTABLE); //Best guess
			modifications.set(UPDATE_PROJECTION_MATRIX, drawtable.getFrameDescriptor().calculateSize() != desc.calculateSize());

			//Evaluate which modifications need to be done
			if(modifications.test(RECREATE_DRAWTABLE)) {
				drawtable = Graphics::Drawtable(drawtable.getVulkan(), desc, depthStencilFmt);
				modifications.set(RECREATE_CLEAR_VALUES); //Best guess
				modifications.set(UPDATE_COLOR_TRANSFER); //Best guess
			}

			if(modifications.test(RECREATE_CLEAR_VALUES)) {
				clearValues = createClearValues(desc, depthStencilFmt);
			}

			if(modifications.test(UPDATE_PROJECTION_MATRIX) && modifications.test(UPDATE_COLOR_TRANSFER)) {
				updateUniforms(cam);
			} else if(modifications.test(UPDATE_PROJECTION_MATRIX)) {
				updateProjectionMatrixUniform(cam);
			} else if(modifications.test(UPDATE_COLOR_TRANSFER)) {
				updateColorTransferUniform();
			}
		}

		void setCamera(const Compositor::Camera& cam) {
			updateProjectionMatrixUniform(cam);
		}

		void addLayer(const LayerData& layer) {
			layers.push_back(layer);
		}

		Video draw() {
			//Cache should have been cleared
			assert(cache.drawCommandBuffers.empty());
			assert(cache.dependencies.empty());

			//Obtain a new frame and command buffer
			auto result = drawtable.acquireFrame();
			auto commandBuffer = commandBufferPool.acquireCommandBuffer();

			//Sort the layers based on their alpha and depth. Stable sort is used to preserve order
			std::stable_sort(
				layers.begin(), layers.end(),
				std::bind(&Open::layerComp, std::cref(*this), std::placeholders::_1, std::placeholders::_2)
			);

			//Obtain all the vulkan command buffers and add them as a dependency
			for(const auto& layer : layers) {
				const auto& commandBuffer = layer.get().getCommandBuffer();
				cache.drawCommandBuffers.push_back(commandBuffer->getCommandBuffer());
				cache.dependencies.push_back(commandBuffer);
			}



			//Begin the commandbuffer
			constexpr vk::CommandBufferBeginInfo cmdBeginInfo(
				vk::CommandBufferUsageFlagBits::eOneTimeSubmit
			);
			commandBuffer->begin(cmdBeginInfo);

			//Bind all descriptors
			commandBuffer->bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
				pipelineLayout,													//Pipeline layout
				DESCRIPTOR_LAYOUT_COMPOSITOR,									//First index
				descriptorSet,													//Descriptor sets
				{}																//Dynamic offsets
			);

			//Draw to the command buffer
			const vk::Rect2D drawRect(
				vk::Offset2D(0, 0),
				Graphics::toVulkan(result->getDescriptor().getResolution())
			);

			result->beginRenderPass(
				commandBuffer->getCommandBuffer(),
				drawRect,
				clearValues, 
				vk::SubpassContents::eSecondaryCommandBuffers //We'll only call execute:
			);

			//Execute all the command buffers gathered from the layers
			if(!cache.drawCommandBuffers.empty()) {
				commandBuffer->execute(cache.drawCommandBuffers); 
			}

			result->endRenderPass(commandBuffer->getCommandBuffer());
			commandBuffer->end();

			//Add dependencies to the command buffer
			commandBuffer->setDependencies(cache.dependencies);

			//Draw to the frame
			result->draw(std::move(commandBuffer));

			//Clear the state. This should not deallocate them
			layers.clear(); 
			cache.drawCommandBuffers.clear();
			cache.dependencies.clear();

			lastFrame = result;
			return result;
		}

	private:
		bool layerComp(const LayerData& a, const LayerData& b) const {
			/*
			 * The strategy will be the following:
			 * 1. Draw the BACKGROUND from bottom to top
			 * 2. Draw the alphaless SCENE backwards, writing and testing depth
			 * 3. Draw the transparent SCENE forwards, writing and testing depth
			 * 4. Draw the FOREGROUND from bottom to top
			 */
			
			if(	a.getRenderingStage() == Compositor::RenderingStage::SCENE &&
				b.getRenderingStage() == Compositor::RenderingStage::SCENE ) 
			{
				//Both are in the scene stage so transparency must be taken into consideration
				assert(a.getRenderingStage() == b.getRenderingStage());

				if(a.getHasAlpha() != b.getHasAlpha()) {
					//Prioritize alphaless drawing
					return a.getHasAlpha() < b.getHasAlpha();
				} else {
					//Both or neither have alpha. Depth must be taken in consideration
					assert(a.getHasAlpha() == b.getHasAlpha());
					const auto hasAlpha = a.getHasAlpha();

					//Calculate the average depth
					const Math::Mat4x4f& projMatrix = Math::Mat4x4f();
					const auto aDepth = (projMatrix * a.getAveragePosition()).z;
					const auto bDepth = (projMatrix * b.getAveragePosition()).z;

					return !hasAlpha
						? aDepth < bDepth
						: aDepth > bDepth;
				}
			} else {
				//Just compare Rendering stages
				return a.getRenderingStage() < b.getRenderingStage();
			}
		}
		
		bool layerCompStencil(const LayerData& a, const LayerData& b) {
			/*
			 * The strategy will be the following:
			 * 1. Draw the alphaless FOREGROUND from top to bottom, writing (0x02) and testing (GEQ) stencil
			 * 2. Draw the alphaless SCENE backwards, writing and testing depth, writing (0x01) and testing (GEQ) stencil
			 * 3. Draw the alphaless BACKGROUND from top to bottom, writing (0x00) and testing (GEQ) stencil
			 * 4. Draw the transparent BACKGROUND from bottom to top, writing (0x00) and testing (GEQ) stencil
			 * 5. Draw the transparent SCENE forwards, writing and testing depth, writing (0x01) and testing (GEQ) stencil
			 * 6. Draw the transparent FOREGROUND from top to bottom, writing (0x02) and testing (GEQ) stencil
			 * 
			 * //FIXME this technique wont preserve background and foreground layer ordering. Somehow layer # should be written
			 * to the depth buffer so that the ordering is preserved. Depth buffer should be cleared from stage to stage.
			 */

			assert(false); (void)(a); (void)(b);//TODO
		}

		void updateProjectionMatrixUniform(const Compositor::Camera& cam) {
			waitRenderCompletion();
			uniformBuffer.waitCompletion(vulkan);

			auto& mtx = *(reinterpret_cast<Math::Mat4x4f*>(uniformBufferLayout[COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX].begin(uniformBuffer.data())));
			mtx = cam.calculateMatrix(drawtable.getFrameDescriptor().calculateSize());

			uniformBuffer.flushData(
				vulkan,
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX].offset(),
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX].size(),
				vulkan.getGraphicsQueueIndex(),
				vk::AccessFlagBits::eUniformRead,
				vk::PipelineStageFlagBits::eVertexShader
			);
		}

		void updateColorTransferUniform() {
			waitRenderCompletion();
			uniformBuffer.waitCompletion(vulkan);
			
			std::memcpy(
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].begin(uniformBuffer.data()),
				colorTransfer.data(),
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].size()
			);

			uniformBuffer.flushData(
				vulkan,
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].offset(),
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].size(),
				vulkan.getGraphicsQueueIndex(),
				vk::AccessFlagBits::eUniformRead,
				vk::PipelineStageFlagBits::eFragmentShader
			);
		}

		void updateUniforms(const Compositor::Camera& cam) {
			waitRenderCompletion();
			uniformBuffer.waitCompletion(vulkan);		
			
			auto& mtx = *(reinterpret_cast<Math::Mat4x4f*>(uniformBufferLayout[COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX].begin(uniformBuffer.data())));
			mtx = cam.calculateMatrix(drawtable.getFrameDescriptor().calculateSize());

			std::memcpy(
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].begin(uniformBuffer.data()),
				colorTransfer.data(),
				uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].size()
			);

			uniformBuffer.flushData(
				vulkan,
				vulkan.getGraphicsQueueIndex(),
				vk::AccessFlagBits::eUniformRead,
				vk::PipelineStageFlagBits::eVertexShader |
				vk::PipelineStageFlagBits::eFragmentShader
			);
		}

		void writeDescriptorSets() {
			const std::array projectionMatrixBuffers = {
				vk::DescriptorBufferInfo(
					uniformBuffer.getBuffer(),												//Buffer
					uniformBufferLayout[COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX].offset(),	//Offset
					uniformBufferLayout[COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX].size()		//Size
				)
			};
			const std::array colorTransferBuffers = {
				vk::DescriptorBufferInfo(
					uniformBuffer.getBuffer(),												//Buffer
					uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].offset(),		//Offset
					uniformBufferLayout[COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER].size()		//Size
				)
			};

			const std::array writeDescriptorSets = {
				vk::WriteDescriptorSet( //Viewport UBO
					descriptorSet,											//Descriptor set
					COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX,				//Binding
					0, 														//Index
					projectionMatrixBuffers.size(),							//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					projectionMatrixBuffers.data(), 						//Buffers
					nullptr													//Texel buffers
				),
				vk::WriteDescriptorSet( //ColorTransfer UBO
					descriptorSet,											//Descriptor set
					COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER,					//Binding
					0, 														//Index
					colorTransferBuffers.size(),							//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					colorTransferBuffers.data(), 							//Buffers
					nullptr													//Texel buffers
				)
			};

			vulkan.updateDescriptorSets(writeDescriptorSets, {});
		}

		void waitRenderCompletion() {
			if(lastFrame) {
				lastFrame->waitDependencies();
			}
		}

		static vk::RenderPass createRenderPass(	const Graphics::Vulkan& vulkan, 
												const Graphics::Frame::Descriptor& frameDesc,
												DepthStencilFormat depthStencilFmt ) 
		{
			return Graphics::Drawtable::getRenderPass(
				vulkan,
				frameDesc,
				depthStencilFmt
			);
		}

		static UniformBufferLayout createUniformBufferLayout(const Graphics::Vulkan& vulkan) {
			const auto& limits = vulkan.getPhysicalDeviceProperties().limits;

			constexpr size_t projectionMatrixOff = 0;
			constexpr size_t projectionMatrixSize = sizeof(glm::mat4);
			
			const size_t colorTansferOff = Utils::align(
				projectionMatrixOff + projectionMatrixSize, 
				limits.minUniformBufferOffsetAlignment
			);
			const size_t colorTansferSize = Graphics::OutputColorTransfer::size();

			return UniformBufferLayout {
				Utils::Area(projectionMatrixOff,	projectionMatrixSize),	//Projection matrix
				Utils::Area(colorTansferOff,		colorTansferSize )		//Color Transfer
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
					COMPOSITOR_DESCRIPTOR_COUNT							//Descriptor count
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
			const std::array layouts {
				getDescriptorSetLayout(vulkan)
			};

			const vk::DescriptorSetAllocateInfo allocInfo(
				pool,													//Pool
				layouts.size(), layouts.data()							//Layouts
			);

			//Allocate it
			vk::DescriptorSet descriptorSet;
			static_assert(layouts.size() == 1);
			const auto result = vulkan.getDevice().allocateDescriptorSets(&allocInfo, &descriptorSet, vulkan.getDispatcher());

			if(result != vk::Result::eSuccess){
				throw Exception("Error allocating descriptor sets");
			}

			return descriptorSet;
		}

		static vk::PipelineLayout createPipelineLayout(const Graphics::Vulkan& vulkan) {			
			//This pipeline layout won't be used to create any pipeline, but it must be compatible with the
			// 1st descriptor set of all the pipelines, so that the color transfer and projection-view matrices
			// are bound.
			static const Utils::StaticId id;

			auto result = vulkan.createPipelineLayout(id);

			if(!result) {
				const std::array layouts {
					getDescriptorSetLayout(vulkan)
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



		static Graphics::Drawtable createDrawtable(	const Graphics::Vulkan& vulkan, 
													const Graphics::Frame::Descriptor& desc,
													DepthStencilFormat depthStencilFmt )
		{
			return Graphics::Drawtable(
				vulkan,
				desc,
				depthStencilFmt
			);
		}

		static Graphics::CommandBufferPool createCommandBufferPool(const Graphics::Vulkan& vulkan) {
			constexpr vk::CommandPoolCreateFlags flags =
				vk::CommandPoolCreateFlagBits::eResetCommandBuffer |	//Command buffers will be reset individually
				vk::CommandPoolCreateFlagBits::eTransient ;				//Command buffers will be reset often

			return Graphics::CommandBufferPool(
				vulkan,
				flags,
				vulkan.getGraphicsQueueIndex(),
				vk::CommandBufferLevel::ePrimary
			);
		}
		
		static std::vector<vk::ClearValue> createClearValues(	const Graphics::Frame::Descriptor& desc,
																DepthStencilFormat depthStencilFmt )
		{
			std::vector<vk::ClearValue> result;

			//Obtain information about the attachments
			const auto coloAttachmentCount = getPlaneCount(desc.getColorFormat());
			const auto hasDepthStencil = DepthStencilFormat::NONE < depthStencilFmt && depthStencilFmt < DepthStencilFormat::COUNT;
			assert(coloAttachmentCount > 0 && coloAttachmentCount <= 4);
			result.reserve(coloAttachmentCount + hasDepthStencil);

			//Add the color attachment clear values
			for(size_t i = 0; i < coloAttachmentCount; ++i) {
				//FIXME write 0.0 to the chroma values in case it is YCbCr
				result.emplace_back(vk::ClearColorValue()); //Default initializer to 0 of floats (Unorm)
			}

			//Add the depth/stencil attachemnt clear values
			if(hasDepthStencil) {
				result.emplace_back(vk::ClearDepthStencilValue(1.0f, 0x00)); //Clear to the far plane
			}

			return result;
		}
	};

	using LayerInput = Signal::Input<LayerDataStream>;
	using Output = Signal::Output<Video>;

	std::reference_wrapper<Compositor> 			owner;

	DepthStencilFormat							depthStencilFormat;
	Compositor::Camera							camera;

	std::vector<LayerInput>						layerIns;
	Output										videoOut;

	std::unique_ptr<Open>						opened;
	bool										hasChanged;

	CompositorImpl(Compositor& comp) 
		: owner(comp)
		, videoOut(std::string(Signal::makeOutputName<Video>()), createPullCallback(this))
	{
	}

	~CompositorImpl() = default;


	void moved(ZuazoBase& base) {
		owner = static_cast<Compositor&>(base);
	}

	void open(ZuazoBase& base) {
		assert(!opened);
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);

		if(compositor.getVideoMode()) {
			opened = Utils::makeUnique<Open>(
				compositor.getInstance().getVulkan(),
				compositor.getVideoMode().getFrameDescriptor(),
				depthStencilFormat,
				camera
			);
		}

		hasChanged = true; //Signal rendering if needed
	}

	void close(ZuazoBase& base) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);
		
		videoOut.reset();
		opened.reset();
	}

	void update() {
		auto& compositor = owner.get();

		if(opened) {
			const bool inputsHaveChanged = std::any_of(
				layerIns.cbegin(), layerIns.cend(),
				[] (const LayerInput& input) -> bool {
					return input.hasChanged();
				}
			);

			if(hasChanged || inputsHaveChanged) {
				if(compositor.getVideoMode()) {
					//Query all the layers
					for(auto& layerIn : layerIns) {
						const auto& layerData = layerIn.pull();

						//Draw only if valid
						if(layerData){
							opened->addLayer(*layerData);
						} 
					}

					videoOut.push(opened->draw());
				} else {
					videoOut.reset();
				}

				//Update the state
				hasChanged = false;
			}
		}
	}

	std::vector<VideoMode> getVideoModeCompatibility() const {
		const auto& compositor = owner.get();
		std::vector<VideoMode> result;

		//Normal formats
		result.emplace_back(
			Utils::MustBe<Rate>(Rate(0, 1)),
			compositor.getInstance().getResolutionSupport(),
			Utils::Any<AspectRatio>(),
			Utils::Any<ColorPrimaries>(),
			Utils::Any<ColorModel>(),
			Utils::MustBe<ColorTransferFunction>(ColorTransferFunction::LINEAR),
			Utils::MustBe<ColorSubsampling>(ColorSubsampling::RB_444),
			Utils::Any<ColorRange>(),
			Graphics::Drawtable::getSupportedFormats(compositor.getInstance().getVulkan())
		);

		//sRGB formats
		result.emplace_back(
			Utils::MustBe<Rate>(Rate(0, 1)),
			compositor.getInstance().getResolutionSupport(),
			Utils::Any<AspectRatio>(),
			Utils::Any<ColorPrimaries>(),
			Utils::MustBe<ColorModel>(ColorModel::RGB),
			Utils::MustBe<ColorTransferFunction>(ColorTransferFunction::LINEAR),
			Utils::MustBe<ColorSubsampling>(ColorSubsampling::RB_444),
			Utils::MustBe<ColorRange>(ColorRange::FULL),
			Graphics::Drawtable::getSupportedSrgbFormats(compositor.getInstance().getVulkan())
		);

		return result;
	}

	void videoModeCallback(VideoBase& base, const VideoMode& videoMode) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);

		if(compositor.isOpen()) {
			const auto isValid = static_cast<bool>(videoMode);

			if(opened && isValid) {
				//Video mode remains valid
				opened->recreate(
					videoMode.getFrameDescriptor(),
					depthStencilFormat,
					camera
				);
			} else if(opened && !isValid) {
				//Video mode is not valid anymore
				opened.reset();
				videoOut.reset();
			} else if(!opened && isValid) {
				//Video mode has become valid
				opened = Utils::makeUnique<Open>(
					compositor.getInstance().getVulkan(),
					videoMode.getFrameDescriptor(),
					depthStencilFormat,
					camera
				);
			}
		}

		hasChanged = true;
	}


	void setLayerCount(size_t count) {
		auto& compositor = owner.get();

		//Unregister all pads as they might be reallocated
		for(auto& pad : layerIns) {
			compositor.removePad(pad);
		}

		//Resize the layer vector
		const auto oldSize = getLayerCount();
		layerIns.resize(count, LayerInput(""));

		//Rename the newly created layers according to their index
		for(size_t i = oldSize; i < layerIns.size(); ++i) {
			layerIns[i].setName(Signal::makeInputName<LayerDataStream>(i));
		}

		//Register all pads again
		for(auto& pad : layerIns) {
			compositor.registerPad(pad);
		}

		hasChanged = true;
	}

	size_t getLayerCount() const {
		return layerIns.size();
	}

	void setCamera(const Compositor::Camera& cam) {
		camera = cam;

		if(opened) {
			opened->setCamera(camera);
		}

		hasChanged = true;
	}

	const Compositor::Camera& getCamera() const {
		return camera;
	}



	static vk::DescriptorSetLayout getDescriptorSetLayout(const Graphics::Vulkan& vulkan) {
		static const Utils::StaticId id;

		auto result = vulkan.createDescriptorSetLayout(id);

		if(!result) {
			//Create the bindings
			const std::array bindings = {
				vk::DescriptorSetLayoutBinding(	//UBO binding
					Open::COMPOSITOR_DESCRIPTOR_PROJECTION_MATRIX,	//Binding
					vk::DescriptorType::eUniformBuffer,				//Type
					1,												//Count
					vk::ShaderStageFlagBits::eVertex,				//Shader stage
					nullptr											//Immutable samplers
				), 
				vk::DescriptorSetLayoutBinding(	//UBO binding
					Open::COMPOSITOR_DESCRIPTOR_COLOR_TRANSFER,		//Binding
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

		assert(result);
		return result;
	}

private:
	Output::PullCallback createPullCallback(CompositorImpl* impl) {
		return [impl] (Output&) {
			impl->owner.get().update();
		};
	}

};



/*
 * Compositor
 */

Compositor::Compositor(	Instance& instance, 
						std::string name, 
						VideoMode videoMode )
	: Utils::Pimpl<CompositorImpl>({}, *this)
	, ZuazoBase(instance, 
				std::move(name),
				PadRef((*this)->videoOut),
				std::bind(&CompositorImpl::moved, std::ref(**this), std::placeholders::_1),
				std::bind(&CompositorImpl::open, std::ref(**this), std::placeholders::_1),
				std::bind(&CompositorImpl::close, std::ref(**this), std::placeholders::_1),
				std::bind(&CompositorImpl::update, std::ref(**this)) )
	, VideoBase(
		std::move(videoMode),
		std::bind(&CompositorImpl::videoModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, Signal::SourceLayout<Video>(makeProxy((*this)->videoOut))
{
	setVideoModeCompatibility((*this)->getVideoModeCompatibility());
}

Compositor::Compositor(Compositor&& other) = default;

Compositor::~Compositor() = default;

Compositor& Compositor::operator=(Compositor&& other) = default;


void Compositor::setLayerCount(size_t count) {
	(*this)->setLayerCount(count);
}

size_t Compositor::getLayerCount() const {
	return (*this)->getLayerCount();
}


void Compositor::setCamera(const Camera& cam) {
	(*this)->setCamera(cam);
}

const Compositor::Camera& Compositor::getCamera() const {
	return (*this)->getCamera();
}



vk::DescriptorSetLayout Compositor::getDescriptorSetLayout(const Graphics::Vulkan& vulkan) {
	return CompositorImpl::getDescriptorSetLayout(vulkan);
}

}
