# Burger-Making Machine

## Overview
The Burger-Making Machine project simulates a burger preparation system using multithreading and shared memory. It allows for the generation of orders, processing of those orders by multiple preparation bands, and management of ingredients.

## Files
- `src/burger_machine.c`: Contains the main logic for the burger-making machine, including the definitions of structures for ingredients, orders, and preparation bands. It implements the functionality for processing orders and managing ingredients.
  
- `src/control.c`: Provides functions to access and control the shared memory used in `burger_machine.c`. This includes starting or stopping preparation bands and checking the status of orders.

- `Makefile`: Used to compile the project. It contains rules for building the `burger_machine` executable from the source files in the `src` directory.

## Building the Project
To build the project, navigate to the root directory of the project and run the following command:

```bash
make
```

This will compile the source files and create the `burger_machine` executable.

## Running the Application
After building the project, you can run the application using the following command:

```bash
./burger_machine <#bands> <buns> <tomato> <onion> <lettuce> <meat> <cheese>
```

### Example
```bash
./burger_machine 3 1 1 0 1 1 1
```

This command starts the burger-making machine with 3 preparation bands and the specified quantities of ingredients.

## Control Interface
The control interface allows you to interact with the running burger-making machine. You can toggle the state of preparation bands, restock ingredients, and exit the application.

## License
This project is licensed under the MIT License. See the LICENSE file for more details.