//
//  ecs.c
//  gl_project
//
//  Created by Scott on 08/09/2022.
//

#include "ecs.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>

typedef unsigned char BYTE;

typedef struct ECSsystem {
	ecsSystemFn			fn;
	ecsComponentQuery	query;
	int					maxThreads;
	int					execOrder;
} ECSsystem;

/**
 * \brief Structure to represent a task the ECS needs to perform after systems finish running.
 * \note Not every member is used by type and thus some might be able to be left uninitialized.
 */
typedef struct ecsTask {
	enum ECS_TASKTYPE {
		ECS_ENTITY_DESTROY,			//! Uses .entity
		ECS_COMPONENTS_DETACH,		//! Uses .entity and .components.mask
		ECS_SYSTEM_CREATE,			//! Uses .system and .components
		ECS_SYSTEM_DESTROY,			//! Uses .system
	} type;
	
	ecsEntityId			entity;		//! relevant entity id
	ECSsystem			system;		//! relevant system function pointer
	ecsComponentQuery	components;	//! relevant components
} ecsTask;

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
static inline int ecsResizeComponents(size_t size);
static inline int ecsResizeComponentType(ECScomponentType* type, size_t size);
static inline int ecsResizeEntities(size_t size);
static inline int ecsResizeSystems(size_t size);
static inline int ecsPushTaskStack(void);
static inline void ecsClearTasks(void);
static inline ECSentityData* ecsFindEntityData(ecsEntityId id);
static inline ECScomponentType* ecsFindComponentType(ecsComponentMask id);
static inline ECSsystem* ecsFindSystem(ecsSystemFn fn);
static inline void* ecsFindComponentFor(ECScomponentType* type, ecsEntityId id);
void ecsPushTask(ecsTask task);


ECSentityList		ecsEntities;
ECScomponentList	ecsComponents;
ECSsystemList		ecsSystems;
ECStaskQueue		ecsTasks;
int					ecsIsInit = 0;


void ecsInit()
{
	assert(!ecsIsInit);

	ecsEntities.nextValidId = 1;
	ecsEntities.begin		= NULL;
	ecsComponents.begin		= NULL;
	ecsSystems.begin		= NULL;
	ecsTasks.begin			= NULL;
	ecsEntities.size = ecsComponents.size = ecsSystems.size = ecsTasks.size = 0;

	ecsIsInit = 1;
}

void ecsTerminate()
{
	assert(ecsIsInit);

	if(ecsEntities.begin)	free(ecsEntities.begin);
	if(ecsSystems.begin)	free(ecsSystems.begin);
	if(ecsTasks.begin)		free(ecsTasks.begin);
	
	if(ecsComponents.begin)
	{
		ECScomponentType* type;
		for(size_t i = 0; i < ecsComponents.size; i++)
		{
			type = ecsComponents.begin + i;
			if(type->begin)
				free(type->begin);
		}
		free(ecsComponents.begin);
	}

	ecsIsInit = 0;
}

ecsComponentMask ecsMakeComponentType(size_t stride)
{
	// avoid going out of bounds on the bitmask
	if (ecsComponents.size == sizeof(ecsComponentMask) * 8) return nocomponent;
	
	ecsComponentMask mask = (0x1ll << ecsComponents.size); // calculate component mask

	// add an element to end of array
	if(ecsResizeComponents(ecsComponents.size + 1))
	{
		ECScomponentType ntype = (ECScomponentType) { // prepare specs of new component type
			.size = 0, .begin = NULL, .id = mask, .stride = (stride + sizeof(ecsEntityId)), .componentSize = stride
		};
		// copy prepared component data
		memmove(ecsComponents.begin + ecsComponents.size-1, &ntype, sizeof(ntype));
		return mask;
	}
	
	return nocomponent;
}

//
// COMPONENTS
//

void ecsSortComponents(ECScomponentType* type)
{
	int swaps;
	void* a;
	void* b;
	ecsEntityId enta;
	ecsEntityId entb;
	void* temp = malloc(type->stride);
	
	// linear sort
	do {
		swaps = 0;
		for(size_t i = 1; i < type->size; ++i)
		{
			a = ((BYTE*)type->begin) + type->stride * (i-1);
			b = ((BYTE*)type->begin) + type->stride * i;
			enta = *(ecsEntityId*)a;
			entb = *(ecsEntityId*)b;
			
			if(enta > entb)
			{
				swaps++;
				memcpy(temp, b, type->stride);
				memcpy(b, a, type->stride);
				memcpy(a, temp, type->stride);
			}
		}
	} while(swaps > 0);
}

void* ecsGetComponentPtr(ecsEntityId e, ecsComponentMask c)
{
	ECScomponentType* ctype = ecsFindComponentType(c);
	
	ecsEntityId* ptr = ecsFindComponentFor(ctype, e);
	if(ptr == NULL) return NULL; // component for e, c combination does not exist
	
	return (void*)(ptr + 1);
}

void ecsAttachComponent(ecsEntityId e, ecsComponentMask c)
{
	ECSentityData* entity = ecsFindEntityData(e);
	ECScomponentType* ctype = ecsFindComponentType(c);
	
	if(ctype == NULL) return;					// component type does not exist
	if(entity == NULL) return;					// no such entity
	if(((entity->mask) & c) != 0) return;		// component already exists
	
	if(ecsResizeComponentType(ctype, ctype->size + 1))
	{
		BYTE* eid = (((BYTE*)ctype->begin) + ((ctype->size-1) * ctype->stride)); // get last item of the list as its entityId block
		memset(eid, 0x0, ctype->stride);		// zero new component
		memmove(eid, &e, sizeof(ecsEntityId));		// set entityId block
		entity->mask |= c;						// register that component was added to entity
		ecsSortComponents(ctype);
	}
}

void ecsAttachComponents(ecsEntityId e, ecsComponentMask q)
{
	ecsComponentMask c; // single component mask
	for(size_t i = 0; i < ecsComponents.size; i++)
	{
		c = (0x1ll << i);
		if((q & c) != 0) // query contains this mask
			ecsAttachComponent(e, c);
	}
}

void ecsDetachComponent(ecsEntityId e, ecsComponentMask c)
{
	ECScomponentType* ctype = ecsFindComponentType(c);

	if(ctype == NULL) return;			// no such component type
	
	ECSentityData* entity = ecsFindEntityData(e);

	if(entity == NULL) return;			// no such entity
	if((entity->mask & c) == 0) return;	// entity does not have component
	
	void* block = ecsFindComponentFor(ctype, e);

	if(block == NULL) return;			// no component block for entity found
	
	void* last = (void*)((BYTE*)(ctype->begin) + (ctype->stride * (ctype->size - 1))); // pointer to last element
	// move last element into to-be-destroyed element
	memmove(block, last, ctype->stride);
	
	// shorten array by one stride
	ecsResizeComponentType(ctype, (ctype->size)-1);
}

void ecsDetachComponents(ecsEntityId e, ecsComponentMask c)
{ ecsPushTask((ecsTask){.type=ECS_COMPONENTS_DETACH, .components={ .mask=c }, .entity=e}); }
void ecsTaskDetachComponents(ecsEntityId e, ecsComponentMask q)
{
	ecsComponentMask id;
	for(size_t i = 0; i < ecsComponents.size; i++)
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
	ecsEntityId id = ecsEntities.nextValidId;
	ecsEntities.nextValidId += 1;
	
	// prepare values
	ECSentityData entity = (ECSentityData) {
		.mask = 0x0, .id = id
	};
	
	// resize entities list
	if(ecsResizeEntities(ecsEntities.size + 1))
	{
		// copy prepared values
		memmove((ecsEntities.begin + ecsEntities.size - 1), &entity, sizeof(entity));
		
		// attach requested components
		ecsAttachComponents(id, components);
		return id;
	}
	return noentity;
}

ecsEntityId ecsGetComponentMask(ecsEntityId entity)
{
	ECSentityData* data = ecsFindEntityData(entity);
	return data != NULL ? data->mask : nocomponent;
}

void ecsDestroyEntity(ecsEntityId e)
{ ecsPushTask((ecsTask){.type=ECS_ENTITY_DESTROY, .entity=e}); }
void ecsTaskDestroyEntity(ecsEntityId e)
{
	ECSentityData* data = ecsFindEntityData(e);
	if(data == NULL) return; // no such entity
	
	// destroy all components owned by entity
	ecsTaskDetachComponents(e, data->mask);
	
	// get the last element of the entities array
	ECSentityData* last = (ecsEntities.begin + ecsEntities.size - 1);
	// copy last into to-be-deleted entity
	memmove(data, last, sizeof(ECSentityData));
	// resize
	ecsResizeEntities(ecsEntities.size - 1);
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

typedef struct ecsRunSystemArgs {
	ecsSystemFn fn;
	ecsEntityId* entities;
	ecsComponentMask* components;
	size_t count;
	float deltaTime;
} ecsRunSystemArgs;

void* ecsRunSystem(void* args)
{
	ecsRunSystemArgs* arg = args;
	arg->fn(arg->entities, arg->components, arg->count, arg->deltaTime);
	return NULL;
}

void ecsRunSystems(float deltaTime)
{
	ECSsystem system;
	ECSentityData entity;
	size_t entityCount = ecsEntities.size;
	
	pthread_t* threads = NULL;
	ecsRunSystemArgs* threadArgs = NULL;
	
	for(size_t i = 0; i < ecsSystems.size; ++i)
	{
		system = ecsSystems.begin[i];
		
		// ECS_NOQUERY systems get run exactly once per ecsRunSystems call
		// with entity and components arguments on NULL
		// and count argument on 0
		if(system.query.comparison == ECS_NOQUERY)
		{
			system.fn(NULL, NULL, 0, deltaTime);
		}
		else
		{
			// look for all entities matching the query
			// first allocate entity and component lists the size of the number of entities
			ecsEntityId* entityList = malloc((entityCount + 1) * sizeof(ecsEntityId));
			ecsComponentMask* componentList = malloc((entityCount + 1) * sizeof(ecsComponentMask));
			assert(entityList != NULL);
			assert(componentList != NULL);

			// then search for entities that match the query
			size_t total = 0;
			for(size_t j = 0; j < entityCount; ++j)
			{
				entity = ecsEntities.begin[j];
				if (matchQuery(system.query, entity.mask))
				{
					assert(total < entityCount);
					entityList[total]		= entity.id;
					componentList[total]	= entity.mask;
					total++;
				}
			}
			
			// dont use threads
			if(system.maxThreads <= 1)
			{
				system.fn(entityList, componentList, total, deltaTime);
			}
			// use threads
			else
			{
				// avoid creating more threads than there are matching entities
				size_t threadCount = system.maxThreads > total ? system.maxThreads : total;
				threads = realloc(threads, threadCount * sizeof(pthread_t));
				threadArgs = realloc(threadArgs, threadCount * sizeof(ecsRunSystemArgs));
				
				// for each thread, create a runsystemargs instance describing it's area of influence
				// then create the thread
				size_t perThreadCount = total / threadCount;
				for(int j = 0; j < threadCount; ++j)
				{
					threadArgs[j].fn = system.fn;
					threadArgs[j].entities = entityList + perThreadCount * j;
					threadArgs[j].components = componentList + perThreadCount * j;
					threadArgs[j].count = perThreadCount;
					threadArgs[j].deltaTime = deltaTime;
					
					pthread_create(threads + j, NULL, &ecsRunSystem, threadArgs + j);
				}
				
				// wait for completion of all threads
				for(int j = 0; j < threadCount; ++j)
				{
					pthread_join(threads[j], NULL);
				}
			}
			
			// clean up
			free(entityList);
			free(componentList);
		}
	}
	if(threads != NULL)
		free(threads);
	if(threadArgs != NULL)
		free(threadArgs);
	
	ecsRunTasks();
}

void ecsSortSystems()
{
	int swaps;
	ECSsystem tmp;
	
	do
	{
		swaps = 0;
		for(int i = 1; i < ecsSystems.size; ++i)
		{
			if(ecsSystems.begin[i-1].execOrder > ecsSystems.begin[i].execOrder)
			{
				memcpy(&tmp, &ecsSystems.begin[i-1], sizeof(ECSsystem));
				memcpy(&ecsSystems.begin[i-1], &ecsSystems.begin[i], sizeof(ECSsystem));
				memcpy(&ecsSystems.begin[i], &tmp, sizeof(ECSsystem));
				swaps++;
			}
		}
	}
	while(swaps > 0);
}

void ecsEnableSystem(ecsSystemFn fn, ecsComponentMask query, ecsQueryComparison comp, int maxThreads, int execOrder)
{
	ecsPushTask((ecsTask)
	{
		.type=ECS_SYSTEM_CREATE,
		.system=(ECSsystem)
		{
			.fn = fn,
			.maxThreads = maxThreads,
			.execOrder = execOrder,
			.query=(ecsComponentQuery)
			{
				.mask=query,
				.comparison=comp
			}
		}
	});
}

void ecsTaskEnableSystem(ECSsystem system)
{
	if(ecsResizeSystems(ecsSystems.size + 1))
	{
		ECSsystem* last = (ecsSystems.begin + ecsSystems.size - 1);
		memcpy(last, &system, sizeof(ECSsystem));
		ecsSortSystems();
	}
}

void ecsDisableSystem(ecsSystemFn fn)
{ ecsPushTask((ecsTask){ .type=ECS_SYSTEM_DESTROY, .system=fn }); }
void ecsTaskDisableSystem(ecsSystemFn fn)
{
	BYTE* dst = ecsFindSystem(fn);
	if(dst == NULL) return; // system not enabled
	
	BYTE* start = dst + sizeof(ECSsystem);
	BYTE* end = ecsSystems.begin + (ecsSystems.size - 1) * sizeof(ECSsystem); // the last item in the systems array
	size_t size = (uintptr_t)end - (uintptr_t)start;
	memmove(dst, start, size);
	// resize array
	ecsResizeSystems(ecsSystems.size - 1);
}

//
// TASKS
//

void ecsPushTask(ecsTask task)
{
	if(ecsPushTaskStack())
	{
		ecsTask* last = ecsTasks.begin + ecsTasks.size - 1;
		memmove(last, &task, sizeof(ecsTask));
	}
}

static inline void ecsRunTask(ecsTask task)
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
		ecsTaskEnableSystem(task.system);
		return;
	case ECS_SYSTEM_DESTROY:
		ecsTaskDisableSystem(task.system.fn);
		return;
	}
}

void ecsRunTasks()
{
	for(size_t i = 0; i < ecsTasks.size; i++)
		ecsRunTask(ecsTasks.begin[i]);
	ecsClearTasks();
}

//
// FIND HELPERS
//

static inline ECScomponentType* ecsFindComponentType(ecsComponentMask id)
{
	for(size_t i = 0; i < ecsComponents.size; ++i)
	{
		if(ecsComponents.begin[i].id == id)
			return (ecsComponents.begin + i);
	}
	return NULL;
}

void* ecsFindComponentFor(ECScomponentType* type, ecsEntityId id)
{
	BYTE* sptr;
	ecsEntityId* eptr;
	size_t l = 0;
	size_t r = type->size - 1;
	size_t m;
	while(l <= r)
	{
		m = floorf((float)(l+r)/2.f);
		sptr = ((BYTE*)type->begin) + m * type->stride; // median element
		eptr = sptr;
		
		// go up
		if(*eptr < id)
			l = m + 1;
		// go down
		else if(*eptr > id)
			r = m - 1;
		// found the correct component
		else if(*eptr == id)
			return sptr + sizeof(ecsEntityId);
	}
	return NULL;
}

static inline ECSentityData* ecsFindEntityData(ecsEntityId id)
{
	for(size_t i = 0; i < ecsEntities.size; ++i)
	{
		if(ecsEntities.begin[i].id == id)
			return (ecsEntities.begin + i);
	}
	return NULL;
}

static inline ECSsystem* ecsFindSystem(ecsSystemFn fn)
{
	for(size_t i = 0; i < ecsSystems.size; ++i)
	{
		if(ecsSystems.begin[i].fn == fn)
			return (ecsSystems.begin + i);
	}
	return NULL;
}

//
// RESIZE HELPERS
//

static inline int ecsResizeSystems(size_t size)
{
	if(size == 0)
	{
		free(ecsSystems.begin);
		ecsSystems.begin = NULL;
		ecsSystems.size = 0;
	}
	else
	{
		ECSsystem* nptr = realloc(ecsSystems.begin, size * sizeof(ECSsystem));
		if(nptr == NULL) return 0;
		
		ecsSystems.size = size;
		ecsSystems.begin = nptr;
	}
	return 1;
}

static inline int ecsPushTaskStack()
{
	size_t size = ecsTasks.size + 1;
	void* nptr = realloc(ecsTasks.begin, size * sizeof(ecsTask));
	if(nptr == NULL) return 0;
	
	ecsTasks.size = size;
	ecsTasks.begin = nptr;
	return 1;
}

static inline void ecsClearTasks()
{
	if(ecsTasks.begin == NULL || ecsTasks.size == 0) return; // no tasks
	
	ecsTasks.size = 0;
	free(ecsTasks.begin);
	ecsTasks.begin = NULL;
}

static inline int ecsResizeEntities(size_t size)
{
	if(size == 0)
	{
		free(ecsEntities.begin);
		ecsEntities.begin = NULL;
		ecsEntities.size = 0;
	}
	else
	{
		ECSentityData* nptr = realloc(ecsEntities.begin, size * sizeof(ECSentityData));
		if(nptr == NULL) return 0;
		
		ecsEntities.size = size;
		ecsEntities.begin = nptr;
	}
	return 1;
}

static inline int ecsResizeComponentType(ECScomponentType* type, size_t size)
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

static inline int ecsResizeComponents(size_t size)
{
	if(size == 0)
	{
		free(ecsComponents.begin);
		ecsComponents.size = 0;
		ecsComponents.begin = NULL;
	}
	else
	{
		ECScomponentType* nptr = realloc(ecsComponents.begin, size * sizeof(ECScomponentType));
		if(nptr == NULL) return 0;
		
		ecsComponents.begin = nptr;
		ecsComponents.size = size;
	}
	return 1;
}
