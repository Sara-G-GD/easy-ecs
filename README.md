# quick-ecs
A simple, easy to integrate entity-component-system library

## Example code
```
#include <ecs.h>

// assigned later, this will store the component mask used to reference this component
ecsComponentMask PositionComponent;
// a sample component storing a x and y coordinate in some 2d space 
typedef struct position_c {
	float x, y;
} position_c;

ecsComponentMask MovementComponent;
// a sample component defining movement
typedef struct movement_c {
	float speed;
} movement_c;

/*
 * System functions in the ECS are given a list of entities and a list of their corresponding component masks.
 * The component masks can be queried by bitwise and-ing the mask with the mask given by ecsMakeComponent.
  * e.g: ( components[i] & PositionComponent ) != 0 to check if PositionComponent exists on entities[i].
 */
void system_movement(ecsEntityid* entities, ecsComponentMask* components, size_t count, float deltaTime)
{
	for(size_t i = 0; i < count; i++)
	{
		//get a reference to the entity's component
		position_c* position = ecsGetComponentPtr(entities[i], PositionComponent);
		movement_c* movement = ecsGetComponentPtr(entities[i], MovementComponent);
		
		// not referencing any specific library, but most gui/windowing/rendering libraries will have some getKey equivalent,
		if(getKey(KEY_LEFT))
			position->x -= movement->speed;
		if (getKey(KEY_RIGHT))
			position->x += movement->speed;
		if(getKey(KEY_UP))
			position->y -= movement->speed;
		if(getKey(KEY_DOWN))
			position->y += movement->speed;
	}	
}

/*
 * It is entirely possible to handle events and rendering through the ECS library,
 * assuming that the context functions and variables are available at this scope.
 * e.g: sf::RenderTarget or SDL_Renderer instances.
 */
void system_handleEvents(ecsEntityid* entities, ecsComponentMask* components, size_t count, float deltaTime);
void system_renderSprites(ecsEntityid* entities, ecsComponentMask* components, size_t count, float deltaTime);

int main()
{
	// initialize your subsystems...

	ecsInit();

	// register a component type and save it's component mask.
	PositionComponent = ecsMakeComponentType(sizeof(position_c));
	MovementComponent = ecsMakeComponentType(sizeof(movement_c));

	// systems will be run in the order they are registered here.
	// systems that are registered using ECS_NOQUERY are run once every frame.	
	// these systems will be passed NULL for the entities and components arguments, 0 for count and deltaTime as normal.
	// the component mask argument of the ecsEnableSystem call is ignored.
	ecsEnableSystem(system_handleEvents, nocomponent, ECS_NOQUERY);
	ecsEnableSystem(system_handleEvents, nocomponent, ECS_NOQUERY);

	// register system_movement as a system.
	// the system will trigger only on entities containing a PositionComponent.
	ecsEnableSystem(system_movement, PositionComponent | MovementComponent, ECS_QUERY_ALL);

	// entities are made by passing a mask of initial components.
	// make an entity with a position component.
	ecsEntityId id = ecsCreateEntity(PositionComponent | MovementComponent);
	{
		// initialize components
		position_c* position = ecsGetComponentPtr(id, PositionComponent);
		position->x = 10.f;
		position->y = 10.f;
		movement_c* movement = ecsGetComponentPtr(id, MovementComponent);
		movement->speed = 1.f;
	}

	int done = 0;
	while(!done)
	{
		// do rendering and event handling if not done using the ECS library...

		// as this ECS implementation was written for games, systems have the requirement of knowing the delta time.
		// calculating delta time is currently left to the end programmer.
		ecsRunSystems(lastTime - currentTime);
	}

	ecsTerminate();

	// terminate subsystems and other cleanup tasks...

	return 0;
}
```
