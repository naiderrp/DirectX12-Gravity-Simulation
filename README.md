DirectX 12 Real-Time Gravity Simulation
======================

## Demo: 10 000 particles

![](img/blur-demo.gif)

## Introduction

The best way to learn compute pipelines is to create a simulation. And the best model to follow is the [Microsoft DirectX Graphics Samples](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master).

Any simulation can be implemented using either a particle-based or a grid-based approach. The former involves storing an array of particles and tracking their velocity and position individually. (Refer to my [previous project](https://github.com/naiderrp/Vulkan-Fluid-Simulation), I explained this model in more details.) On the other hand, the grid-based approach entails working with sampled images, that is considered a more advanced graphics programming technique. The implemented simulation focuses on the first approach, which I found to be easier for those who are getting started with DirectX compute shaders.

## Environment

* Intel Integrated UHD Graphics 620
* Windows 10
* Visual Studio 2022

## Physics Background

The only formula you'll need is [Newton's law of universal gravitation](https://en.wikipedia.org/wiki/Newton%27s_law_of_universal_gravitation). What is more, the notation taught in school would be enough. No derivatives, integrals, or complex equations. It's simple yet incredibly elegant!

![](img/looking-around.gif)

## Tracking Velocity

Each particle's color is interpolated from blue to yellow based on its velocity magnitude. As you can see, the most intense gravitational interaction is observed at the center of the particle cluster.

To make a demonstration clearer, I disabled a blur effect. 

![](img/no-blur-demo.gif)

## Boosting Performance

The current implementation is not that optimized. The FPS counter sits at around 40, which is decent for 10 000 particles. However, even with an integrated GPU, we can achieve better results. 

The array of particles is interleaved, which presents a bottleneck. Thus, the primary improvement needed is transitioning to a flat data structure. A data structure is considered flat if its elements are stored together in a contiguous piece of storage. Flat data structures offer an advantage in cache locality, making them more efficient to traverse. Again, you can read about the results [here](https://github.com/naiderrp/Vulkan-Fluid-Simulation). Guess I could double performance and add a few thousands more particles in the simulation.

Storing your data as a tightly-packed array is, for sure, in the best-practice list. 

Also, I can't help mentioning [this](https://gpuopen.com/learn/nbody-directx-12-async-compute-edition/) edition. Exploring the author's optimizations on the Microsoft gravity simulation could give the low-down on increasing FPS while working with Microsoft samples.

![](img/moving-away.gif)
 
## Resources

The following links may be useful for your own project.

* [Microsoft DirectX samples](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/UWP)
* [Multi-engine n-body gravity simulation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/multi-engine-n-body-gravity-simulation)
* [AMD performance guide](https://gpuopen.com/learn/rdna-performance-guide/)
