/*
 * This example shows how instantiate a compositor
 * Requires: 
 * - zuazo-window
 * 
 * How to compile:
 * c++ 01\ -\ VideoSurface.cpp -std=c++17 -Wall -Wextra -lzuazo -lzuazo-ffmpeg -lzuazo-window -lzuazo-compositor -lavutil -lavformat -lavcodec -lswscale -lglfw -ldl -lpthread
 */


#include <zuazo/Instance.h>
#include <zuazo/Modules/Window.h>
#include <zuazo/Modules/Compositor.h>
#include <zuazo/Sources/FFmpegClip.h>
#include <zuazo/Consumers/Window.h>
#include <zuazo/Processors/Compositor.h>
#include <zuazo/Processors/Layers/VideoSurface.h>

#include <mutex>
#include <iostream>

int main(int argc, const char* argv[]) {
	if(argc < 2) {
		std::cerr << "Usage: " << *argv << " <video_file>" << std::endl;
		std::terminate();
	}

	//Instantiate Zuazo as usual. Note that compositor module is loaded 
	Zuazo::Instance::ApplicationInfo appInfo (
		"Compositor 01",							//Application's name
		Zuazo::Version(0, 1, 0),					//Application's version
		Zuazo::Verbosity::GEQ_INFO,					//Verbosity 
		{ 	Zuazo::Modules::Window::get(), 			//Modules
			Zuazo::Modules::Compositor::get() }
	);
	Zuazo::Instance instance(std::move(appInfo));
	std::unique_lock<Zuazo::Instance> lock(instance);

	//Construct the desired video mode
	const Zuazo::VideoMode videoMode = Zuazo::makeVideoMode(Zuazo::Rate(60, 1)); //Just specify the desired rate

	//Create a input
	Zuazo::Sources::FFmpegClip clip(
		instance,
		"Video source",
		Zuazo::VideoMode::ANY,
		std::string(argv[1])
	);

	clip.open();
	clip.setRepeat(Zuazo::ClipBase::Repeat::REPEAT);
	clip.play();

	//Construct the window object
	Zuazo::Consumers::Window window(
		instance, 						//Instance
		"Output Window",				//Layout name
		videoMode,						//Video mode limits
		Zuazo::Math::Vec2i(1280, 720),	//Window size (in screen coordinates)
		Zuazo::Consumers::Window::NO_MONITOR //No monitor
	);

	//Open the window (now becomes visible)
	window.open();

	//Create a videomode for the compositor
	auto compositorVideoMode = Zuazo::PixelFormats::RENDER_OPTIMAL_8.intersect(Zuazo::Resolutions::FHD);
	compositorVideoMode.setColorPrimaries(Zuazo::Utils::MustBe<Zuazo::ColorPrimaries>(Zuazo::ColorPrimaries::BT709));
	compositorVideoMode.setColorModel(Zuazo::Utils::MustBe<Zuazo::ColorModel>(Zuazo::ColorModel::RGB));

	//Create the compositor
	Zuazo::Processors::Compositor compositor(
		instance,
		"Compositor",
		compositorVideoMode,
		Zuazo::Utils::Any<Zuazo::DepthStencilFormat>()
	);

	compositor.open();

	//Create the video surface
	Zuazo::Processors::Layers::VideoSurface surface(
		instance,
		"Video Surface",
		&compositor,
		Zuazo::Math::Vec2f(768, 768)
	);

	surface.setScalingMode(Zuazo::ScalingMode::BOXED);
	surface.open();

	//Route the signals
	compositor.setLayers({surface});
	window << compositor;
	surface << clip;

	//Done!
	lock.unlock();
	getchar();
	lock.lock();
}