#if !defined(workqueue_h)
#define workqueue_h

class Work {
    public:
        virtual void work() = 0;
        virtual char const *name() = 0;
        virtual void complete() { delete this; }
        virtual void error() { delete this; }
    protected:
        virtual ~Work() {}
};

bool start_work_queue(int nthreads);
bool add_work(Work *work);
int get_num_working();
void wait_for_all_work_to_complete();
void stop_work_queue();

#endif  //  workqueue_h
