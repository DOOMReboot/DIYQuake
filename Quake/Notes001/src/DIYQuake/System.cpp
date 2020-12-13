#include "System.h"

#include <iostream>
#include <SDL.h>

std::unique_ptr<System> System::m_pInstance = nullptr;

System* System::GetInstance()
{
	if (!m_pInstance)
	{
		m_pInstance = std::unique_ptr<System>(new System());
	}

	return m_pInstance.get();
}

System::~System()
{
}

System::System()
{
}

void System::Init()
{
	SDLInit();
}

void System::SDLInit()
{
	//Initialize SDL
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0)
	{
		std::cout << "ERROR: SDL failed to initialize! SDL_Error: " << SDL_GetError() << std::endl;
	}
}
