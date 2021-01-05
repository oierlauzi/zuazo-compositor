/*
 * This example shows how to buil a simple video switcher
 * 
 * How to compile:
 * c++ 00\ -\ Instantiating.cpp -std=c++17 -Wall -Wextra -lzuazo -lzuazo-window -lzuazo-ffmpeg -lzuazo-compositor -lglfw -ldl -lpthread -lavutil -lavformat -lavcodec -lswscale
 */


#include <zuazo/Instance.h>
#include <zuazo/Player.h>
#include <zuazo/Modules/Window.h>
#include <zuazo/Consumers/WindowRenderer.h>
#include <zuazo/Processors/Compositor.h>
#include <zuazo/Processors/Layers/VideoSurface.h>
#include <zuazo/Sources/FFmpegClip.h>

#include <mutex>
#include <iostream>

static Zuazo::Consumers::WindowRenderer createWindow(Zuazo::Instance& instance) {
	//Construct the desired parameters
	const Zuazo::VideoMode videoMode(
		Zuazo::Utils::MustBe<Zuazo::Rate>(Zuazo::Rate(25, 1)), //Just specify the desired rate
		Zuazo::Utils::Any<Zuazo::Resolution>(),
		Zuazo::Utils::Any<Zuazo::AspectRatio>(),
		Zuazo::Utils::Any<Zuazo::ColorPrimaries>(),
		Zuazo::Utils::Any<Zuazo::ColorModel>(),
		Zuazo::Utils::Any<Zuazo::ColorTransferFunction>(),
		Zuazo::Utils::Any<Zuazo::ColorSubsampling>(),
		Zuazo::Utils::Any<Zuazo::ColorRange>(),
		Zuazo::Utils::Any<Zuazo::ColorFormat>()	
	);

	const Zuazo::Utils::Limit<Zuazo::DepthStencilFormat> depthStencil(
		Zuazo::Utils::MustBe<Zuazo::DepthStencilFormat>(Zuazo::DepthStencilFormat::NONE) //Not interested in the depth buffer
	);

	const auto windowSize = Zuazo::Math::Vec2i(1280, 720);

	const auto& monitor = Zuazo::Consumers::WindowRenderer::NO_MONITOR; //Not interested in the full-screen mode

	//Construct the window objects
	Zuazo::Consumers::WindowRenderer window(
		instance, 						//Instance
		"Output Window",				//Layout name
		videoMode,						//Video mode limits
		depthStencil,					//Depth buffer limits
		windowSize,						//Window size (in screen coordinates)
		monitor							//Monitor for setting fullscreen
	);

	window.setWindowName(window.getName());
	window.setResizeable(false); //Disable resizeing, as extra care needs to be taken
	window.open();

	return window;
}

static Zuazo::Processors::Layers::VideoSurface createOuputLayer(Zuazo::Consumers::WindowRenderer& window) {
	Zuazo::Processors::Layers::VideoSurface videoSurface(
		window.getInstance(),
		"Output Video Surface",
		&window,
		window.getVideoMode().getResolutionValue()
	);

	videoSurface.setScalingMode(Zuazo::ScalingMode::STRETCH);
	videoSurface.setScalingFilter(Zuazo::ScalingFilter::NEAREST);
	videoSurface.open();

	return videoSurface;
}

static Zuazo::Sources::FFmpegClip createVideoClip(Zuazo::Instance& instance, std::string path) {
	Zuazo::Sources::FFmpegClip videoClip(
		instance,
		"Input Video",
		Zuazo::VideoMode::ANY,
		std::move(path)
	);
	videoClip.setRepeat(Zuazo::Sources::FFmpegClip::Repeat::REPEAT);
	videoClip.play();
	videoClip.open();

	return videoClip;
}

static Zuazo::Processors::Compositor createCompositor(Zuazo::Instance& instance) {
	//Construct the desired parameters
	const Zuazo::VideoMode videoMode(
		Zuazo::Utils::Any<Zuazo::Rate>(),
		Zuazo::Utils::MustBe<Zuazo::Resolution>(Zuazo::Resolution(1280, 720)),
		Zuazo::Utils::MustBe<Zuazo::AspectRatio>(Zuazo::AspectRatio(1, 1)),
		Zuazo::Utils::MustBe<Zuazo::ColorPrimaries>(Zuazo::ColorPrimaries::IEC61966_2_1),
		Zuazo::Utils::MustBe<Zuazo::ColorModel>(Zuazo::ColorModel::RGB),
		Zuazo::Utils::MustBe<Zuazo::ColorTransferFunction>(Zuazo::ColorTransferFunction::LINEAR),
		Zuazo::Utils::MustBe<Zuazo::ColorSubsampling>(Zuazo::ColorSubsampling::RB_444),
		Zuazo::Utils::MustBe<Zuazo::ColorRange>(Zuazo::ColorRange::FULL),
		Zuazo::Utils::MustBe<Zuazo::ColorFormat>(Zuazo::ColorFormat::R16fG16fB16f)	
	);

	const Zuazo::Utils::Limit<Zuazo::DepthStencilFormat> depthStencil(
		Zuazo::Utils::MustBe<Zuazo::DepthStencilFormat>(Zuazo::DepthStencilFormat::D16) //16 bit integer depth buffer
	);

	Zuazo::Processors::Compositor compositor(
		instance,
		"Compositor",
		videoMode,
		depthStencil
	);
	compositor.open();

	return compositor;
}

static Zuazo::Math::Vec2f getRandomVec2f() {
	return Zuazo::Math::Vec2f(
		static_cast<float>(rand()) / static_cast<float>(RAND_MAX),
		static_cast<float>(rand()) / static_cast<float>(RAND_MAX)
	);
}

int main(int argc, const char** argv) {
	if(argc != 2) {
		std::cerr << "Usage: " << *argv << " <video file>" << std::endl;
	}

	//Instantiate Zuazo as usual. Note that we're loading the Window module
	Zuazo::Instance::ApplicationInfo appInfo(
		"Compositor Example 00",					//Application's name
		Zuazo::Version(0, 1, 0),					//Application's version
		Zuazo::Verbosity::GEQ_INFO,					//Verbosity 
		{ Zuazo::Modules::Window::get() }			//Modules
	);
	Zuazo::Instance instance(std::move(appInfo));
	std::unique_lock<Zuazo::Instance> lock(instance);

	//Configure the output
	auto window = createWindow(instance);
	auto outputLayer = createOuputLayer(window);
	window.setLayers({outputLayer});

	//Create the input clip
	auto videoClip = createVideoClip(instance, argv[1]);
	Zuazo::Player videoClipPlayer(instance, &videoClip);
	videoClipPlayer.enable();

	//Create the compositor and its layers
	auto compositor = createCompositor(instance);
	std::vector<Zuazo::Processors::Layers::VideoSurface> layers;

	//Configure keyboard callback for the window
	const auto keyCallback = [&compositor, &videoClip, &layers] (	Zuazo::Consumers::WindowRenderer&, 
																	Zuazo::KeyboardKey key, 
																	Zuazo::KeyEvent event, 
																	Zuazo::KeyModifiers)
	{
		if(event == Zuazo::KeyEvent::PRESS) {
			//We only care for presses
			switch(key) {
			case Zuazo::KeyboardKey::SPACE:
			case Zuazo::KeyboardKey::ENTER:
				std::cout << "Adding layer #" << compositor.getLayers().size() << std::endl;
				{
					const auto size = Zuazo::Math::Vec2f(compositor.getVideoMode().getResolutionValue());

					layers.emplace_back(
						compositor.getInstance(),
						"Compositor Layer" + Zuazo::toString(compositor.getLayers().size()),
						&compositor,
						getRandomVec2f() * size / 2.0f
					);
					auto transform = layers.back().getTransform();
					transform.setPosition(Zuazo::Math::Vec3f(Zuazo::Math::lerp(-size/2.0f, size/2.0f, getRandomVec2f()), 0.0f));
					layers.back().setTransform(transform);
					layers.back().open();
					layers.back() << videoClip;

					const std::vector<Zuazo::Processors::Compositor::LayerRef> newLayers(
						layers.cbegin(), layers.cend()
					);
					compositor.setLayers(newLayers);
				}
				
				break;

			case Zuazo::KeyboardKey::BACKSPACE:
			case Zuazo::KeyboardKey::DELETE:
				if(layers.size()) {
					layers.pop_back();
					std::cout << "Removing a layer #" << compositor.getLayers().size() << std::endl;

					const std::vector<Zuazo::Processors::Compositor::LayerRef> newLayers(
						layers.cbegin(), layers.cend()
					);
					compositor.setLayers(newLayers);
				}

				break;

			default:
				//Unknown action
				break;
			}
		}
	};
	window.setKeyboardCallback(keyCallback);

	std::cout << "Compositor's video-mode:" << std::endl;
	std::cout << compositor.getVideoMode() << std::endl;

	//Route the signals
	outputLayer << compositor;

	//Done!
	lock.unlock();
	getchar();
	lock.lock();
}