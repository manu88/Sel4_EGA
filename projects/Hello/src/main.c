#include <stdio.h>
#include <vspace/vspace.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <sel4platsupport/io.h>

#include <sel4platsupport/bootinfo.h>
#include <sel4platsupport/arch/io.h> //jm
#include <platsupport/chardev.h> //jm

#include <sel4utils/vspace.h>

#include <allocman/bootstrap.h>
#include <allocman/vka.h>

//#include <utils/util.h>


/* dimensions of virtual memory for the allocator to use */
#define ALLOCATOR_VIRTUAL_POOL_SIZE ((1 << seL4_PageBits) * 100)

/* static memory for the allocator to bootstrap with */
#define ALLOCATOR_STATIC_POOL_SIZE ((1 << seL4_PageBits) * 10)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

/* static memory for virtual memory bootstrapping */
static sel4utils_alloc_data_t data;


#define EGA_TEXT_FB_BASE 0xB8000
#define MODE_WIDTH 80
#define MODE_HEIGHT 25

typedef struct 
{
    seL4_BootInfo *info;
    /* An initialised vka that may be used by the test. */
    vka_t vka;
    /* virtual memory management interface */
    vspace_t vspace;
    /* abtracts over kernel version and boot environment */
    simple_t simple;
    /* platsupport I/O */
    ps_io_ops_t io_ops;
} Env;


static Env _env;
static ps_chardevice_t devKeyboard;

/* initialise our runtime environment */
static void
init_env(Env *env)
{
    allocman_t *allocman;
    UNUSED reservation_t virtual_reservation;
    UNUSED int error;

    /* create an allocator */
    allocman = bootstrap_use_current_simple(&env->simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    assert(allocman);

    /* create a vka (interface for interacting with the underlying allocator) */
    allocman_make_vka(&env->vka, allocman);

    /* create a vspace (virtual memory management interface). We pass
     * boot info not because it will use capabilities from it, but so
     * it knows the address and will add it as a reserved region */
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&env->vspace,
                                                           &data, simple_get_pd(&env->simple), &env->vka, env->info);

    /* fill the allocator with virtual memory */
    void *vaddr;
    virtual_reservation = vspace_reserve_range(&env->vspace,
                                               ALLOCATOR_VIRTUAL_POOL_SIZE, seL4_AllRights, 1, &vaddr);
    assert(virtual_reservation.res);
    bootstrap_configure_virtual_pool(allocman, vaddr,
                                     ALLOCATOR_VIRTUAL_POOL_SIZE, simple_get_pd(&env->simple));

    //initialize env->io_ops
    error = sel4platsupport_new_io_ops( env->vspace, env->vka, &env->io_ops);
    assert(error == 0);
}


/*
 * Map video RAM with support libs.
 */
void* mapVideoRam(Env *env) 
{
     void* vram = ps_io_map(&env->io_ops.io_mapper, EGA_TEXT_FB_BASE,
  				0x1000, false, PS_MEM_NORMAL);
    assert(vram != NULL);
    return vram;
}

void writeVideoRam(uint16_t* vram, int row) 
{
    printf("VRAM mapped at: 0x%x\n", (unsigned int) vram);

    const int width = MODE_WIDTH;
    for (int col = 0; col < 80; col++) 
    {
	vram[width * row + col] =  ('0' + col) | (col << 8);
    }
}

int main(void)
{

     _env.info = platsupport_get_bootinfo();

	simple_default_init_bootinfo(&_env.simple, _env.info);

    init_env(&_env);

    printf("Init keyboard...\n");

    struct ps_io_ops    opsIO;
    sel4platsupport_get_io_port_ops(&opsIO.io_port_ops, &_env.simple , &_env.vka);

    ps_chardevice_t *devKeyboardRet;

    devKeyboardRet = ps_cdev_init(PC99_KEYBOARD_PS2, &opsIO, &devKeyboard);

    if (devKeyboardRet == NULL)
    {
	printf("Error init keyboard\n");
	return 1;
    }

    printf("Map EGA mem\n");

    void* vram = mapVideoRam(&_env);
    printf("Mapped EGA \n");

    for(int i=0;i<MODE_HEIGHT;i++)
    {
    	writeVideoRam((uint16_t*)vram, i);
    }


    for(;;) 
    {
        int c = ps_cdev_getchar(&devKeyboard);
        if (c != EOF) 
    	{
            printf("You typed [%c]\n", c);
        }
        fflush(stdout);
    }


    return 0;
}
