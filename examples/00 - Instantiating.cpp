/*
 * This example shows how instantiate a compositor
 * Requires: 
 * - zuazo-window
 * 
 * How to compile:
 * c++ 00\ -\ Instantiating.cpp -std=c++17 -Wall -Wextra -lzuazo -lzuazo-window -lzuazo-compositor -lglfw -ldl -lpthread
 */


#include <zuazo/Instance.h>
#include <zuazo/Modules/Window.h>
#include <zuazo/Modules/Compositor.h>
#include <zuazo/Consumers/Window.h>
#include <zuazo/Processors/Compositor.h>

#include <mutex>
#include <iostream>

int main() {
	//Instantiate Zuazo as usual. Note that compositor module is loaded 
	Zuazo::Instance::ApplicationInfo appInfo (
		"Compositor 00",							//Application's name
		Zuazo::Version(0, 1, 0),					//Application's version
		Zuazo::Verbosity::GEQ_INFO,					//Verbosity 
		{ 	Zuazo::Modules::Window::get(), 			//Modules
			Zuazo::Modules::Compositor::get() }
	);
	Zuazo::Instance instance(std::move(appInfo));
	std::unique_lock<Zuazo::Instance> lock(instance);

	//Construct the desired video mode
	const Zuazo::VideoMode videoMode = Zuazo::makeVideoMode(Zuazo::Rate(60, 1)); //Just specify the desired rate

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

	auto compositorVideoMode = Zuazo::PixelFormats::RENDER_OPTIMAL_8.intersect(Zuazo::Resolutions::FHD);
	compositorVideoMode.setColorPrimaries(Zuazo::Utils::MustBe<Zuazo::ColorPrimaries>(Zuazo::ColorPrimaries::BT709));
	compositorVideoMode.setColorModel(Zuazo::Utils::MustBe<Zuazo::ColorModel>(Zuazo::ColorModel::BT709));
	Zuazo::Processors::Compositor compositor(
		instance,
		"Compositor",
		compositorVideoMode
	);

	compositor.open();
	std::cout << compositor.getVideoMode() << std::endl;

	window << compositor;



	//Done!
	lock.unlock();
	getchar();
	lock.lock();


}