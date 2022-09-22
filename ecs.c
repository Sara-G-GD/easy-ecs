//
//  components.c
//  gl_project
//
//  Created by Scott on 08/09/2022.
//

#include "ecs.h"
#include <assert.h>
#include <stdio.h>

typedef unsigned char BYTE;

typedef struct ECSsystem {
	ecsSystemFn	fn;
	ecsComponentQuery	query;
} ECSsystem;

typedef struct ECSentityData {
	ecsEntityId		id;
	ecsComponentMask	mask;
} ECSentityData;

typedef struct ECScomponentType {
	ecsComponentMask		id;
	size_t			stride;
	size_t			componentSize;
	size_t			size;
	void*			begin;
} ECScomponentType;

typedef struct ECScomponentList {
	size_t				size;
	ECScomponentType*	begin;
} ECScomponentList;

typedef struct ECSentityList {
	size_t		size;
	size_t		nextValidId;
	ECSentityData* begin;
} ECSentityList;

typedef struct ECSsystemList {
	size_t		size;
	ECSsystem*	begin;
} ECSsystemList;

typedef struct ECStaskQueue {
	size_t size;
	ecsTask* begin;
} ECStaskQueue;

// forward declare helper functions
static inline int resizeComponents(size_t size);
static inline int resizeComponentType(ECScomponentType* type, size_t size);
static inline int resizeEntities(size_t size);
static inline int resizeSystems(size_t size);
static inline int pushTask(void);
static inline void clearTasks(void);
static inline ECSentityData* findEntityData(ecsEntityId id);
static inline ECScomponentType* findComponentType(ecsComponentMask id);
static inline ECSsystem* findSystem(ecsSystemFn fn);
static inline void* findComponentFor(ECScomponentType* type, ecsEntityId id);


ECSentityList		entities;
ECScomponentList	components;
ECSsystemList		systems;
ECStaskQueue		tasks;


void ecsInit()
{
	entities.nextValidId = 1;
	entities.begin		= NULL;
	components.begin	= NULL;
	systems.begin		= NULL;
	tasks.begin			= NULL;
	entities.size = components.size = systems.size = tasks.size = 0;
}

void ecsTerminate()
{
	if(entities.begin)	free(entities.begin);
	if(systems.begin)	free(systems.begin);
	if(tasks.begin)		free(tasks.begin);
	
	if(components.begin)
	{
		ECScomponentType* type;
		for(size_t i = 0; i < components.size; i++)
		{
			type = components.begin + i;
			if(type->begin)
				free(type->begin);
		}
		free(components.begin);
	}
}

ecsComponentMask ecsMakeComponentType(size_t stride)
{
	// avoid going out of bounds on the bitmask
	if (components.size == sizeof(ecsComponentMask) * 8) return nocomponent;
	
	ecsComponentMask mask = (0x1ll << components.size); // calculate component mask

	// add an element to end of array
	if(resizeComponents(components.size + 1))
	{
		ECScomponentType ntype = (ECScomponentType) { // prepare specs of new component type
			.size = 0, .begin = NULL, .id = mask, .stride = (stride + sizeof(ecsEntityId)), .componentSize = stride
		};
		// copy prepared component data
		memmove(components.begin + components.size-1, &ntype, sizeof(ntype));
		return mask;
	}
	
	return nocomponent;
}

//
// COMPONENTS
//

void* ecsGetComponentPtr(ecsEntityId e, ecsComponentMask c)
{
	ECScomponentType* ctype = findComponentType(c);
	
	ecsEntityId* ptr = findComponentFor(ctype, e);
	if(ptr == NULL) return NULL; // component for e, c combination does not exist
	
	return (void*)(ptr + 1);
}

void ecsAttachComponent(ecsEntityId e, ecsComponentMask c)
{
	ECSentityData* entity = findEntityData(e);
	ECScomponentType* ctype = findComponentType(c);
	
	if(ctype == NULL) return;					// component type does not exist
	if(entity == NULL) return;					// no such entity
	if(((entity->mask) & c) != 0) return;		// component already exists
	
	if(resizeComponentType(ctype, ctype->size + 1))
	{
		BYTE* eid = (((BYTE*)ctype->begin) + ((ctype->size-1) * ctype->stride)); // get last item of the list as its entityId block
		memset(eid, 0x0, ctype->stride);		// zero new component
		memmove(eid, &e, sizeof(ecsEntityId));		// set entityId block
		entity->mask |= c;						// register that component was added to entity
	}
}

void ecsAttachComponents(ecsEntityId e, ecsComponentMask q)
{
	ecsComponentMask c; // single component mask
	for(size_t i = 0; i < components.size; i++)
	{
		c = (0x1ll << i);
		if((q & c) != 0) // query contains this mask
			ecsAttachComponent(e, c);
	}
}

void ecsDetachComponent(ecsEntityId e, ecsComponentMask c)
{
	ECScomponentType* ctype = findComponentType(c);

	if(ctype == NULL) return;			// no such component type
	
	ECSentityData* entity = findEntityData(e);

	if(entity == NULL) return;			// no such entity
	if((entity->mask & c) == 0) return;	// entity does not have component
	
	void* block = findComponentFor(ctype, e);

	if(block == NULL) return;			// no component block for entity found
	
	void* last = (void*)((BYTE*)(ctype->begin) + (ctype->stride * (ctype->size - 1))); // pointer to last element
	// move last element into to-be-destroyed element
	memmove(block, last, ctype->stride);
	
	// shorten array by one stride
	resizeComponentType(ctype, (ctype->size)-1);
}

void ecsDetachComponents(ecsEntityId e, ecsComponentMask c)
{ ecsPushTask((ecsTask){.type=ECS_COMPONENTS_DETACH, .components={ .mask=c }, .entity=e}); }
void ecsTaskDetachComponents(ecsEntityId e, ecsComponentMask q)
{
	ecsComponentMask id;
	for(size_t i = 0; i < components.size; i++)
	{
		id = (0x1ll << i);
		if((q & id) != 0) // query contains component type at i
			ecsDetachComponent(e, id);
	}
}

//
// ENTITIES
//

ecsEntityId ecsCreateEntity(ecsComponentMask components)
{
	// register an id that is unique for the runtime of the ecs
	ecsEntityId id = entities.nextValidId;
	entities.nextValidId += 1;
	
	// prepare values
	ECSentityData entity = (ECSentityData) {
		.mask = 0x0, .id = id
	};
	
	// resize entities list
	if(resizeEntities(entities.size + 1))
	{
		// copy prepared values
		memmove((entities.begin + entities.size - 1), &entity, sizeof(entity));
		
		// attach requested components
		ecsAttachComponents(id, components);
		return id;
	}
	return noentity;
}

ecsEntityId ecsGetComponentMask(ecsEntityId entity)
{
	ECSentityData* data = findEntityData(entity);
	return data != NULL ? data->mask : nocomponent;
}

void ecsDestroyEntity(ecsEntityId e)
{ ecsPushTask((ecsTask){.type=ECS_ENTITY_DESTROY, .entity=e}); }
void ecsTaskDestroyEntity(ecsEntityId e)
{
	ECSentityData* data = findEntityData(e);
	if(data == NULL) return; // no such entity
	
	// destroy all components owned by entity
	ecsTaskDetachComponents(e, data->mask);
	
	// get the last element of the entities array
	ECSentityData* last = (entities.begin + entities.size - 1);
	// copy last into to-be-deleted entity
	memmove(data, last, sizeof(ECSentityData));
	// resize
	resizeEntities(entities.size - 1);
}

//
// SYSTEMS
//

int matchQuery(ecsComponentQuery query, ecsComponentMask mask)
{
	if(query.comparison == ECS_QUERY_ANY)
		return (mask & query.mask) != 0;
	else if(query.comparison == ECS_QUERY_ALL)
		return (mask & query.mask) == query.mask;
	return 0;
}
void ecsRunSystems(float deltaTime)
{
	ECSsystem system;
	ECSentityData entity;
	size_t entityCount = entities.size;
	for(size_t i = 0; i < systems.size; ++i)
	{
		system = systems.begin[i];
		// ECS_NOQUERY systems get run exactly once per ecsRunSystems call
		if(system.query.comparison == ECS_NOQUERY)
		{
			system.fn(NULL, NULL, 0, deltaTime);
		}
		else
		{
			ecsEntityId* entityList = malloc((entityCount + 1) * sizeof(ecsEntityId));
			ecsComponentMask* componentList = malloc((entityCount + 1) * sizeof(ecsComponentMask));
			assert(entityList != NULL);
			assert(componentList != NULL);

			size_t total = 0;
			for(size_t j = 0; j < entityCount; ++j)
			{
				entity = entities.begin[j];
				if (matchQuery(system.query, entity.mask))
				{
					assert(total < entityCount);
					entityList[total]		= entity.id;
					componentList[total]	= entity.mask;
					total++;
				}
			}
			system.fn(entityList, componentList, total, deltaTime);
			free(entityList);
			free(componentList);
		}
	}
	
	ecsRunTasks();
}

void ecsEnableSystem(ecsSystemFn fn, ecsComponentMask query, ecsQueryComparison comp)
{ ecsPushTask((ecsTask){ .type=ECS_SYSTEM_CREATE, .system=fn, .components=(ecsComponentQuery){ .mask=query, .comparison=comp} }); }
void ecsTaskEnableSystem(ecsSystemFn fn, ecsComponentQuery query)
{
	if(resizeSystems(systems.size + 1))
	{
		ECSsystem* last = (systems.begin + systems.size - 1);
		last->query = query;
		last->fn = fn;
	}
}

void ecsDisableSystem(ecsSystemFn fn)
{ ecsPushTask((ecsTask){ .type=ECS_SYSTEM_DESTROY, .system=fn }); }
void ecsTaskDisableSystem(ecsSystemFn fn)
{
	ECSsystem* s = findSystem(fn);
	if(s == NULL) return; // system not enabled
	
	ECSsystem* last = systems.begin + systems.size - 1; // the last item in the systems array
	// copy last into to-be-disabled
	memmove(s, last, sizeof(ECSsystem));
	// resize array
	resizeSystems(systems.size - 1);
}

//
// TASKS
//

void ecsPushTask(ecsTask task)
{
	if(pushTask())
	{
		ecsTask* last = tasks.begin + tasks.size - 1;
		memmove(last, &task, sizeof(ecsTask));
	}
}

void ecsRunTask(ecsTask task)
{
	switch(task.type)
	{
	default: return;
		
	case ECS_ENTITY_DESTROY:
		ecsTaskDestroyEntity(task.entity);
		return;

	case ECS_COMPONENTS_DETACH:
		ecsTaskDetachComponents(task.entity, task.components.mask);
		return;
		
	case ECS_SYSTEM_CREATE:
		ecsTaskEnableSystem(task.system, task.components);
		return;
	case ECS_SYSTEM_DESTROY:
		ecsTaskDisableSystem(task.system);
		return;
	}
}

void ecsRunTasks()
{
	for(size_t i = 0; i < tasks.size; i++)
		ecsRunTask(tasks.begin[i]);
	clearTasks();
}

//
// FIND HELPERS
//

static inline ECScomponentType* findComponentType(ecsComponentMask id)
{
	for(size_t i = 0; i < components.size; ++i)
	{
		if(components.begin[i].id == id)
			return (components.begin + i);
	}
	return NULL;
}

static inline void* findComponentFor(ECScomponentType* type, ecsEntityId id)
{
	BYTE* sptr;
	ecsEntityId* eptr;
	for(size_t i = 0; i < type->size; ++i)
	{
		sptr = ((BYTE*)(type->begin) + (type->stride * i)); // get entityId ptr to element i
		eptr = (ecsEntityId*)sptr;
		if((*eptr) == id)
			return sptr;
	}
	return NULL;
}

static inline ECSentityData* findEntityData(ecsEntityId id)
{
	for(size_t i = 0; i < entities.size; ++i)
	{
		if(entities.begin[i].id == id)
			return (entities.begin + i);
	}
	return NULL;
}

static inline ECSsystem* findSystem(ecsSystemFn fn)
{
	for(size_t i = 0; i < systems.size; ++i)
	{
		if(systems.begin[i].fn == fn)
			return (systems.begin + i);
	}
	return NULL;
}

//
// RESIZE HELPERS
//

static inline int resizeSystems(size_t size)
{
	if(size == 0)
	{
		free(systems.begin);
		systems.begin = NULL;
		systems.size = 0;
	}
	else
	{
		ECSsystem* nptr = realloc(systems.begin, size * sizeof(ECSsystem));
		if(nptr == NULL) return 0;
		
		systems.size = size;
		systems.begin = nptr;
	}
	return 1;
}

static inline int pushTask()
{
	size_t size = tasks.size + 1;
	void* nptr = realloc(tasks.begin, size * sizeof(ecsTask));
	if(nptr == NULL) return 0;
	
	tasks.size = size;
	tasks.begin = nptr;
	return 1;
}

static inline void clearTasks()
{
	if(tasks.begin == NULL || tasks.size == 0) return; // no tasks
	
	tasks.size = 0;
	free(tasks.begin);
	tasks.begin = NULL;
}

static inline int resizeEntities(size_t size)
{
	if(size == 0)
	{
		free(entities.begin);
		entities.begin = NULL;
		entities.size = 0;
	}
	else
	{
		ECSentityData* nptr = realloc(entities.begin, size * sizeof(ECSentityData));
		if(nptr == NULL) return 0;
		
		entities.size = size;
		entities.begin = nptr;
	}
	return 1;
}

static inline int resizeComponentType(ECScomponentType* type, size_t size)
{
	if(size == 0)
	{
		if (type->begin == NULL) return 1;

		free(type->begin);
		type->begin = NULL;
		type->size = 0;
	}
	else
	{
		void* nptr = realloc(type->begin, size * (type->stride));
		if(nptr == NULL) return 0;
		
		type->size = size;
		type->begin = nptr;
	}
	return 1;
}

static inline int resizeComponents(size_t size)
{
	if(size == 0)
	{
		free(components.begin);
		components.size = 0;
		components.begin = NULL;
	}
	else
	{
		ECScomponentType* nptr = realloc(components.begin, size * sizeof(ECScomponentType));
		if(nptr == NULL) return 0;
		
		components.begin = nptr;
		components.size = size;
	}
	return 1;
}
