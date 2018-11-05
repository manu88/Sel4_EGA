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
#include <vka/capops.h>
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


#define KEY_BADGE 10
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

//    cspacepath_t keyboardirq_path;
} Env;


typedef struct _chardev_t {
    /* platsupport char device */
    ps_chardevice_t dev;
    /* IRQHandler cap (with cspace path) */
    cspacepath_t handler;
    /* endpoint cap (with cspace path) device is waiting for IRQ */
    cspacepath_t ep;
} chardev_t;


static Env _env;

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

    sel4platsupport_get_io_port_ops(&env->io_ops.io_port_ops, &env->simple , &env->vka);

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

// creates IRQHandler cap "handler" for IRQ "irq"
static void
get_irqhandler_cap(int irq, cspacepath_t* handler)
{
    seL4_CPtr cap;
    // get a cspace slot
    UNUSED int err = vka_cspace_alloc(&_env.vka, &cap);
    assert(err == 0);

    // convert allocated cptr to a cspacepath, for use in
    // operations such as Untyped_Retype
    vka_cspace_make_path(&_env.vka, cap, handler);

    // exec seL4_IRQControl_Get(seL4_CapIRQControl, irq, ...)
    // to get an IRQHandler cap for IRQ "irq"
    err = simple_get_IRQ_handler(&_env.simple, irq, *handler);
    assert(err == 0);
}

// finalize device setup
// hook up endpoint (dev->ep) with IRQ of char device (dev->dev)
void set_devEp(chardev_t* dev) {
    // Loop through all IRQs and get the one device needs to listen to
    // We currently assume there it only needs one IRQ.
    int irq;
    for (irq = 0; irq < 256; irq++) {
        if (ps_cdev_produces_irq(&dev->dev, irq)) {
            break;
        }
    }
    printf ("irq=%d\n", irq);

    //create IRQHandler cap
    get_irqhandler_cap(irq, &dev->handler);

    /* Assign AEP to the IRQ handler. */
    UNUSED int err = seL4_IRQHandler_SetNotification(
            dev->handler.capPtr, dev->ep.capPtr);
    assert(err == 0);

    //read once: not sure why, but it does not work without it, it seems
    ps_cdev_getchar(&dev->dev);
    err = seL4_IRQHandler_Ack(dev->handler.capPtr);
    assert(err == 0);
}

static void handle_cdev_event( chardev_t* dev) 
{
    for (;;) 
    {
        //int c = __arch_getchar();
        int c = ps_cdev_getchar(&dev->dev);
        if (c == EOF) {
            //read till we get EOF
            break;
        }
        printf("You typed [%c]\n", c);
    }

    UNUSED int err = seL4_IRQHandler_Ack(dev->handler.capPtr);
    assert(err == 0);
}


int main(void)
{
    int error;
     _env.info = platsupport_get_bootinfo();

    simple_default_init_bootinfo(&_env.simple, _env.info);

    init_env(&_env);

    printf("Init keyboard...\n");

    struct ps_io_ops    opsIO;
    sel4platsupport_get_io_port_ops(&opsIO.io_port_ops, &_env.simple , &_env.vka);


    printf("Map EGA mem\n");

    void* vram = mapVideoRam(&_env);
    printf("Mapped EGA \n");

    for(int i=0;i<MODE_HEIGHT;i++)
    {
    	writeVideoRam((uint16_t*)vram, i);
    }


    vka_object_t mainEndpoint = {0};

    error = vka_alloc_endpoint(&_env.vka, &mainEndpoint);
    assert(error == 0);

    vka_object_t notification = {0};

    error = vka_alloc_notification(&_env.vka, &notification);
    assert(error == 0);


    /* bind the notification to the current thread */
    error = seL4_TCB_BindNotification(seL4_CapInitThreadTCB, notification.cptr);
    assert(error == 0);

    // create space for notification
    cspacepath_t notification_path;
    vka_cspace_make_path(&_env.vka, notification.cptr, &notification_path);

    // create a badged notif for the keyboard
    //cspacepath_t badged_notification;
    //error = vka_cspace_alloc_path(&_env.vka, &badged_notification);
    //assert(error == 0);

    chardev_t keyboard;

    error = vka_cspace_alloc_path(&_env.vka, &keyboard.ep);
    assert(error == 0);

    printf("MINT\n");
    error = vka_cnode_mint(&keyboard.ep,& notification_path/*badged_notification*/, seL4_AllRights, KEY_BADGE);
    assert(error == 0); 


    ps_chardevice_t *ret;

    ret = ps_cdev_init(PC99_KEYBOARD_PS2, &_env.io_ops, &keyboard.dev);
    assert(ret != NULL);
    
    set_devEp(&keyboard);

    for(;;) 
    {


        seL4_Word sender_badge = 0;
        seL4_MessageInfo_t message;
        seL4_Word label;

        message = seL4_Recv(mainEndpoint.cptr, &sender_badge);

	if (sender_badge == KEY_BADGE)
	{
		handle_cdev_event( &keyboard);
	}
	else 
	{
    	    printf("Got OTHER sender_badge %li\n",sender_badge);

	}
/*
        int c = ps_cdev_getchar(&devKeyboard);
        if (c != EOF) 
    	{
            printf("You typed [%c]\n", c);
        }
        fflush(stdout);
*/
    }


    return 0;
}
