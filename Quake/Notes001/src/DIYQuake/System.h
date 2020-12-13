#pragma once

#include <memory>

class System
{
public:
	static System* GetInstance();
	~System();
	void Init();

protected:
	System();

	static std::unique_ptr <System> m_pInstance;

	void SDLInit();
};

