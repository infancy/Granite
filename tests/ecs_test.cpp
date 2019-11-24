#include "ecs.hpp"
#include <iostream>

using namespace Granite;
using namespace std;

struct AComponent : ComponentBase
{
	GRANITE_COMPONENT_TYPE_DECL(AComponent)
	AComponent(int v_)
		: v(v_)
	{
	}
	int v;
};

struct BComponent : ComponentBase
{
	GRANITE_COMPONENT_TYPE_DECL(BComponent)
	BComponent(int v_)
		: v(v_)
	{
	}
	int v;
};

struct CComponent : ComponentBase
{
	GRANITE_COMPONENT_TYPE_DECL(CComponent)
	CComponent(int v_)
		: v(v_)
	{
	}
	int v;
};

template <uint64_t hash>
uint64_t compile_hash()
{
	return hash;
}

int main()
{
	//static_assert(::Util::compile_time_fnv1("FNV Hash Test") == 0xcce9980dbe477f71, "xx");

	std::cout << ::Util::compile_time_fnv1("tseT hsaH VNF") << '\n';
	std::cout << compile_hash<::Util::compile_time_fnv1("tseT hsaH VNF")>() << '\n';


	EntityPool pool;
	auto a = pool.create_entity();
	a->allocate_component<AComponent>(10);
	a->allocate_component<BComponent>(20);

	auto &group_ab = pool.get_component_group<AComponent, BComponent>();
	auto &group_ba = pool.get_component_group<BComponent, AComponent>();
	auto &group_bc = pool.get_component_group<BComponent, CComponent>();

	a->allocate_component<AComponent>(40);

	for (auto &e : group_ab)
		LOGI("AB: %d, %d\n", get<0>(e)->v, get<1>(e)->v);
	for (auto &e : group_ba)
		LOGI("BA: %d, %d\n", get<0>(e)->v, get<1>(e)->v);
	for (auto &e : group_bc)
		LOGI("BC: %d\n", get<0>(e)->v);
}