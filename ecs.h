//
//  ecs.h
//  gl_project
//
//  Created by Scott on 08/09/2022.
//

#ifndef ecs_h
#define ecs_h

#include <stddef.h>
#include <memory.h>
#include <stdlib.h>


#if __cplusplus
extern "C" {
#endif

typedef unsigned long long ecsEntityId;
typedef unsigned long long ecsComponentMask;

typedef void (*ecsSystemFn)(ecsEntityId*, ecsComponentMask*, size_t, float);

#define noentity		((ecsEntityId)0x0)
#define nocomponent		((ecsComponentMask)0x0)
#define anycomponent	((ecsComponentMask)~0x0)

typedef enum ECSqueryComparison {
	ECS_NOQUERY = 0x0,
	ECS_QUERY_ANY,
	ECS_QUERY_ALL,
} ecsQueryComparison;

typedef struct ecsComponentQuery {
	ecsQueryComparison comparison;
	ecsComponentMask mask;
} ecsComponentQuery;

void ecsInit(void);

/**
 * \brief Allocates a component list for a component type of stride bytes.
 * \param stride The number of bytes to allocate for each component.
 */
ecsComponentMask ecsMakeComponentType(size_t stride);
#define ecsRegisterComponent(__type) ecsMakeComponentType(sizeof(__type))

/**
 * \brief Get a pointer to a component attached to entity.
 * \param entity The entity to find a component of.
 * \param component The component type to find.
 * \returns A pointer to a component if found.
 * \returns NULL if entity does not contain the given component.
 */
void* ecsGetComponentPtr(ecsEntityId entity, ecsComponentMask component);

/**
 * \brief Assigns a new entity id.
 * \param components  A component query referencing the components to add to the new object.
 * \returns The id used to reference the newly created entity.
 * \returns NULL if allocation failed
 */
ecsEntityId ecsCreateEntity(ecsComponentMask components);

/**
 * \brief Gets the component mask for an entity.
 * \param entity the entity to get the mask for.
 * \returns the ecsComponentMask for entity.
 */
ecsEntityId ecsGetComponentMask(ecsEntityId entity);

/**
 * \brief Destroys an entity and all associated components
 * \param entity The id of the entity to destroy.
 */
void ecsDestroyEntity(ecsEntityId entity);

/**
 * \brief Attaches one or more components.
 * \param entity The entity to attach the new components to.
 * \param components Bitmask of the componentId's to attach.
 */
void ecsAttachComponents(ecsEntityId entity, ecsComponentMask components);

/**
 * \brief Detaches one or more components.
 * \param entity The entity to detach components from.
 * \param components Bitmask of the components to detach.
 */
void ecsDetachComponents(ecsEntityId entity, ecsComponentMask components);

/**
 * \brief Enables a function to act as a system for entities matching the given query.
 * \param func The function to call when query is met.
 * \param components The required components to run this system.
 * \param comparison The type of requirement components represent. one of { ECS_QUERY_ANY ; ECS_QUERY_ALL }.
 * \note
 * When comparison=ECS_QUERY_ALL the system will run only when all of the masked components are present on an entity.
 * \note
 * When comparison=ECS_QUERY_ANY the system will run for all entities where any of the masked components are present.
 */
void ecsEnableSystem(ecsSystemFn func, ecsComponentMask components, ecsQueryComparison comparison, int maxThreads, int executionOrder);

/**
 * \brief Disables a function acting as a system.
 * \param func Pointer to the function to disable.
 */
void ecsDisableSystem(ecsSystemFn func);

/**
 * \brief Run currently enabled systems.
 * \note Implicitly calls ecsRunTasks after completion.
 */
void ecsRunSystems(float deltaTime);

/**
 * \brief Run queued tasks.
 */
void ecsRunTasks(void);

/**
 * \brief Terminate the ECS and clean up allocated resources.
 */
void ecsTerminate(void);

#if __cplusplus
}
#endif

#endif /* ecs_h */
