#include <Egss.h>

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{

	}
	~TestEnv()
	{

	}
};



Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}