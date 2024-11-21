#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#define MAX_TRAINS 100
#define NANOSECOND_CONVERSION 1e9

#include <stdarg.h>



typedef struct {
    int train_id;
    int priority;   // 0 = low, 1 = high
    char direction; // 'E' for east, 'W' for west
    int load_time;  // Loading time in tenths of seconds
    int track_time; // Time to cross the track in tenths of seconds
    bool loaded;    // loading status
    double loading_completion_time; // Time when loading completed
    bool has_crossed; // Tracks if the train has crossed    
} Train;

typedef struct Node {
    Train* train;
    struct Node* next;
} Node;

typedef struct {
    Node* head;
    Node* tail;
    pthread_mutex_t mutex;  // Protects access to the queue
    pthread_cond_t cond;    // Condition variable for the queue  // added change here
} Queue;

// Queue functions
void init_queue(Queue* queue) {
    queue->head = NULL;
    queue->tail = NULL;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL); // added change here
}

// Convert timespec to seconds
double timespec_to_seconds(struct timespec *ts) {
    return ((double) ts->tv_sec) + (((double) ts->tv_nsec) / NANOSECOND_CONVERSION);
}

void enqueue(Queue* queue, Train* train) {


    Node* new_node = (Node*)malloc(sizeof(Node));
    new_node->train = train;
    new_node->next = NULL;

    pthread_mutex_lock(&queue->mutex); // Lock the queue

    if (queue->tail == NULL) { // Queue is empty
        queue->head = new_node;
        queue->tail = new_node;
    } else {
        queue->tail->next = new_node;
        queue->tail = new_node;
    }

    pthread_cond_signal(&queue->cond); // added change here

    pthread_mutex_unlock(&queue->mutex); // Unlock the queue
}


void print_queue_state(Queue* queue) {
    // Lock the queue mutex to ensure safe access
    pthread_mutex_lock(&queue->mutex);

    printf("Queue state:\n");
    if (queue->head == NULL) {
        printf("The queue is empty.\n");
    } else {
        Node* current = queue->head;
        while (current != NULL) {
            Train* train = current->train; // Access the train in the current node
            printf("Train ID: %d, Direction: %c, Loading Time: %d, Crossing Time: %d\n",
                   train->train_id, train->direction, train->load_time, train->track_time);
            current = current->next;
        }
    }

    // Unlock the queue mutex
    pthread_mutex_unlock(&queue->mutex);
}


void remove_train(Queue* queue, int train_id) {
    pthread_mutex_lock(&queue->mutex);




    Node* current = queue->head;
    Node* previous = NULL;

    while (current != NULL) {
        if (!current->train) {
            printf("Encountered invalid node.\n");
            break;
        }


        if (current->train->train_id == train_id) {

            if (previous == NULL) {
                // We're at the head of the queue
                queue->head = current->next;
            } else {
                previous->next = current->next;
            }
            if (current == queue->tail) {
                // We're at the tail of the queue
                queue->tail = previous;
            }

            free(current);  // Free the removed node

            break;
        }

        previous = current;
        current = current->next;
    }



    pthread_mutex_unlock(&queue->mutex);
}


// Global variables
Queue* waiting_queue;
pthread_mutex_t start_timer;
pthread_cond_t train_ready_to_load;
bool ready_to_load = false;

char last_train_direction = 'W'; // Default to West
struct timespec start_time = { 0 };

// Removed loading_cond and loading_mutex // added change here

volatile bool shutdown = false;


void print_timestamped_message(const char* message, int train_id, char direction) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    double elapsed = timespec_to_seconds(&current_time) - timespec_to_seconds(&start_time);
    int hours = (int)elapsed / 3600;
    int minutes = ((int)elapsed % 3600) / 60;
    double seconds = elapsed - (hours * 3600) - (minutes * 60);

    printf("%02d:%02d:%04.1f Train %d %s %s\n",
           hours, minutes, seconds,
           train_id,
           message,
           direction == 'E' ? "East" : "West");
}

Train* find_train(Queue* waiting_queue, char last_train_direction,int* consecutive_count) {
    // No need to lock here; dispatcher already holds the mutex // added change here
    Node* current = waiting_queue->head;
    Train* selected_train = NULL;
    Train* opposite_direction_train = NULL;

    while (current != NULL) {
        // Select based on priority first
        if (selected_train == NULL || current->train->priority > selected_train->priority) {
            // Update the selected train
            opposite_direction_train = NULL; // Reset opposite train
            selected_train = current->train;
        } else if (current->train->priority == selected_train->priority) {
            // Handle same priority scenario
            if (current->train->direction == selected_train->direction) {
                // Both in the same direction
                if (current->train->load_time < selected_train->load_time) {
                    selected_train = current->train; // Current loaded first
                }
            } else {
                // Different directions
                if (last_train_direction == 'W' && current->train->direction == 'E') {
                    // Current is Eastbound
                    selected_train = current->train; // Select Eastbound
                } else if (last_train_direction == 'E' && current->train->direction == 'W') {
                    // Current is Westbound
                    if (selected_train == NULL || opposite_direction_train == NULL) {
                        selected_train = current->train; // Prioritize first opposer
                    }
                }
            }
        }

        // Check if there's a train in the opposite direction
        if (current->train->direction != last_train_direction) {
            opposite_direction_train = current->train; // Potential opposite train
        }

        current = current->next; // Move to the next train
    }

    // Avoid starvation logic: Check if we can give a chance to opposite direction train
 //   if (selected_train != NULL &&
   //     opposite_direction_train != NULL &&
     //   selected_train->direction == last_train_direction) {
        // If selected train is in the same direction, prioritize the opposite
       // selected_train = opposite_direction_train;
    //}

        if (*consecutive_count >= 3 && opposite_direction_train != NULL) {
        selected_train = opposite_direction_train;
        *consecutive_count = 0;
    } else if (selected_train != NULL && selected_train->direction == last_train_direction) {
        (*consecutive_count)++;
    } else {
        *consecutive_count = 1;
    }


    return selected_train; // Return the selected train
}

typedef struct {  // added change here
    Queue* waiting_queue;
    int total_trains;
} DispatcherArgs;

void* dispatcher(void* arg) {
    DispatcherArgs* args = (DispatcherArgs*)arg; // added change here
    Queue* waiting_queue = args->waiting_queue;  // added change here
    int total_trains = args->total_trains;       // added change here
    int processed_trains = 0;                    // added change here

     int consecutive_count = 1; // Track consecutive trains in the same direction

    bool track_busy = false; // Flag to track if the track is busy
    pthread_mutex_t track_mutex; // Mutex for track access
    pthread_cond_t track_cond; // Condition variable for track availability
    // Removed re-declaration of last_train_direction // added change here

    // Initialize mutex and condition variable
    pthread_mutex_init(&track_mutex, NULL);
    pthread_cond_init(&track_cond, NULL);

    while (1) {
        pthread_mutex_lock(&track_mutex); // Lock the track mutex

        // Check if there are any loaded trains in the waiting queue
        pthread_mutex_lock(&waiting_queue->mutex); // Lock queue access // added change here
        while (waiting_queue->head == NULL) {
            if (shutdown) { // added change here
                pthread_mutex_unlock(&waiting_queue->mutex); // added change here
                pthread_mutex_unlock(&track_mutex); // added change here
                pthread_mutex_destroy(&track_mutex); // added change here
                pthread_cond_destroy(&track_cond);   // added change here
                return NULL; // added change here
            }
            // No trains are in the queue; wait for a train to be loaded
            pthread_cond_wait(&waiting_queue->cond, &waiting_queue->mutex); // added change here
        }

        // Find the train that deserves to go first
        Train* next_train = find_train(waiting_queue, last_train_direction,&consecutive_count);
        pthread_mutex_unlock(&waiting_queue->mutex); // Unlock queue mutex // added change here

        // Check if the track is busy
        while (track_busy) {
            // Wait for the track to become free
            pthread_cond_wait(&track_cond, &track_mutex);
        }

        // Simulate crossing
        print_timestamped_message("is ON the main track going", next_train->train_id, next_train->direction);
        track_busy = true; // Mark the track as busy

        // Simulate the track crossing time
        usleep((next_train->track_time / 10.0) * 1e6); // Convert seconds to microseconds


        // Update the last direction
        last_train_direction = next_train->direction;

        //print_queue_state(waiting_queue);
        // Remove the train from the queue after crossing
        remove_train(waiting_queue, next_train->train_id);


        print_timestamped_message("is OFF the main track after going", next_train->train_id, next_train->direction);

        processed_trains++; // Increment the count of processed trains // added change here

        track_busy = false; // Mark the track as free

        // Signal that the track is now free
        pthread_cond_signal(&track_cond);

        pthread_mutex_unlock(&track_mutex); // Unlock the track mutex
    }
}
    // Train thread function


// Train thread function
void* train_thread(void* arg) {
    Train* original_pointer = (Train*)arg;

    // Wait until the start signal is given
    pthread_mutex_lock(&start_timer);
    while (!ready_to_load) {
        pthread_cond_wait(&train_ready_to_load, &start_timer);
    }
    pthread_mutex_unlock(&start_timer);

    // Calculate loading time in seconds
    double loading_duration = original_pointer->load_time / 10.0;

    // Simulate the loading time by sleeping
    usleep((int)(loading_duration * 1e6)); // Convert seconds to microseconds

    // Get the time when loading is completed
    struct timespec ready_time;
    clock_gettime(CLOCK_MONOTONIC, &ready_time);

    // Enqueue the train after loading


    // Format and print readiness message
    double elapsed = timespec_to_seconds(&ready_time) - timespec_to_seconds(&start_time);
    int hours = (int)elapsed / 3600;
    int minutes = ((int)elapsed % 3600) / 60;
    double seconds = elapsed - (hours * 3600) - (minutes * 60);
    printf("%02d:%02d:%04.1f Train %d is ready to go %s\n",
           hours, minutes, seconds,
           original_pointer->train_id,
           original_pointer->direction == 'E' ? "East" : "West");

    // Removed unnecessary signaling and mutex // added change here
     enqueue(waiting_queue, original_pointer);
    pthread_exit(NULL);
}

void start_trains(void) {
    pthread_mutex_lock(&start_timer);
    ready_to_load = true;
    pthread_cond_broadcast(&train_ready_to_load);
    pthread_mutex_unlock(&start_timer);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    FILE* file = fopen(argv[1], "r");
    if (file == NULL) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    Train trains[MAX_TRAINS];
    int train_count = 0;

    waiting_queue = (Queue*)malloc(sizeof(Queue));
    if (waiting_queue == NULL) {
        perror("Failed to allocate memory for waiting queue");
        exit(EXIT_FAILURE);
    }

    // Initialize the waiting queue
    init_queue(waiting_queue);

    // Read the input file
    char direction;
    int load_time, track_time;

    while (fscanf(file, " %c %d %d\n", &direction, &load_time, &track_time) == 3) {
        Train* train = &trains[train_count];
        train->train_id = train_count;
        train->load_time = load_time;
        train->track_time = track_time;

        // Determine priority and direction based on input character
        if (direction == 'e' || direction == 'E') {
            train->direction = 'E'; // East direction
            train->priority = isupper(direction) ? 1 : 0; // Priority based on direction case
        } else if (direction == 'w' || direction == 'W') {
            train->direction = 'W'; // West direction
            train->priority = isupper(direction) ? 1 : 0; // Priority based on direction case
        }

        train_count++;
        if (train_count >= MAX_TRAINS) {
            fprintf(stderr, "Too many trains! Max supported is %d.\n", MAX_TRAINS);
            break;
        }
    }

    fclose(file);

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    // printf("Program start time: %f\n", timespec_to_seconds(&start_time)); // Optional

    DispatcherArgs dispatcher_args; // added change here
    dispatcher_args.waiting_queue = waiting_queue; // added change here
    dispatcher_args.total_trains = train_count;    // added change here

         FILE *output_file = freopen("output.txt", "w", stdout);
    if (output_file == NULL) {
        perror("Error opening output file");
        exit(EXIT_FAILURE);
    }

    pthread_t dispatcher_thread;

    if (pthread_create(&dispatcher_thread, NULL, dispatcher, (void*)&dispatcher_args) != 0) { // added change here
        perror("Failed to create dispatcher thread");
        free(waiting_queue); // Clean up
        return 1;
    }

    pthread_t threads[MAX_TRAINS];
    for (int i = 0; i < train_count; i++) {
        if (pthread_create(&threads[i], NULL, train_thread, (void*)&trains[i]) != 0) {
            perror("Failed to create thread");
            // Clean up previously created threads and exit
            for (int j = 0; j < i; j++) {
                pthread_cancel(threads[j]);
            }
            free(waiting_queue); // Clean up
            exit(EXIT_FAILURE);
        }
    }

    // Signal to start loading trains
    start_trains();

    // Open the file


    // Join train threads
    for (int i = 0; i < train_count; i++) {
        pthread_join(threads[i], NULL);
    }

    shutdown = true; // added change here

    // Wake up dispatcher to exit if waiting // added change here
    pthread_mutex_lock(&waiting_queue->mutex); // added change here
    pthread_cond_broadcast(&waiting_queue->cond); // added change here
    pthread_mutex_unlock(&waiting_queue->mutex); // added change here

    pthread_join(dispatcher_thread, NULL);

    // Clean up
    free(waiting_queue);
        fclose(output_file);

    return 0;
}
