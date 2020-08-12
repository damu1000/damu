//Standalone code similar to Uintah "Hypre Solver" task invoked from Uintah example "solver test 1"
//Compile line:
//  mpicxx hypre_test.cc -std=c++11 -fopenmp -I/home/sci/damodars/installs/2_hypre-2.11.0_omp_original/src/hypre/include/ -L/home/sci/damodars/installs/2_hypre-2.11.0_omp_original/src/hypre/lib -lHYPRE -g -O2
//run as:
//  export OMP_NUM_THREADS=16
//	mpirun -np 4 ./a.out 64 4
//  This will create a grid of 64 cube cell divided among 4 cube number of patches. Thus each patch will be of size 16 cube. 16 omp threads per rank
//  For simplicity assuming cubical domain with same cells and patches in every dimension.
/*

Albion:
CPU Only
export MPICH_CXX=g++
export HYPRE_PATH=/home/damodars/uintah_kokkos_dev/hypre_kokkos/src/build

mpicxx hypre_test.cc -std=c++11 -I$(HYPRE_PATH)/include/ -L$(HYPRE_PATH)/lib -lHYPRE -g -O3

mpicxx 2_hypre_gpu.cc -std=c++11 -I/home/damodars/uintah_kokkos_dev/hypre_kokkos/src/build/include/ -L/home/damodars/uintah_kokkos_dev/hypre_kokkos/src/build/lib -lHYPRE -g -O3 -I$KOKKOS_PATH/build/include -L$KOKKOS_PATH/build/lib -lkokkos --expt-extended-lambda -o gpu -arch=sm_52 


 mpicxx hypre_cpu_ep.cc -std=c++11 -I/home/damodars/hypre_ep/hypre_ep/src/build/include/ -I/home/damodars/hypre_ep/hypre_ep/src/ -L/home/damodars/hypre_ep/hypre_ep/src/build/lib -lHYPRE -I/home/damodars/install/libxml2-2.9.7/build/include/libxml2 -L/home/damodars/install/libxml2-2.9.7/build/lib -lxml2  -g -O3 -o 2_ep -fopenmp

 */

//#include <cuda_profiler_api.h>


#include<chrono>
#include <ctime>
#include<cmath>
#include<vector>
#include<mpi.h>
#include<stdio.h>
#include<chrono>
#include <ctime>
#include<iostream>
#include<string.h>
// hypre includes
#include <_hypre_struct_mv.h>
#include <_hypre_utilities.h>
#include <HYPRE_struct_ls.h>
#include <krylov.h>
//#include <Kokkos_Core.hpp>
#include <omp.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include "io.h"


//typedef Kokkos::Serial KernelSpace;
//typedef Kokkos::Cuda KernelSpace;

struct Stencil4{
	double p, n, e, t;
};

//typedef Kokkos::View<double*, Kokkos::LayoutRight, KernelSpace::memory_space> ViewDouble;
//typedef Kokkos::View<Stencil4*, Kokkos::LayoutRight, KernelSpace::memory_space> ViewStencil4;

double tolerance = 1.e-25;
char **argv;
int argc;

typedef struct IntVector
{
	int value[3]; //x: 0, y:1, z:2. Loop order z, y, x. x changes fastest.
} IntVector;

int get_affinity() {
	int core=-1;
	cpu_set_t mask;
	long nproc, i;

	if (sched_getaffinity(0, sizeof(cpu_set_t), &mask) == -1) {
		perror("sched_getaffinity");
		assert(false);
	} else {
		nproc = sysconf(_SC_NPROCESSORS_ONLN);
		for (i = 0; i < nproc; i++) {
			core++;
			//printf("%d ", CPU_ISSET(i, &mask));
			if(CPU_ISSET(i, &mask))
				break;
		}
		//printf("affinity: %d\n", core);
	}
	return core;
}

//patches numbers are always assigned serially. EP assignment logic may vary.
//index into g_patch_ep indicates patch id, values indicates assigned EP.
std::vector<int> g_patch_ep;
std::vector<int> g_ep_superpatch; //ep to superpatch mapping
int g_superpatches[3];

//as of now using simple logic of serial assignment
void assignPatchToEP(xmlInput input, int size, int num_of_threads){
	int num_of_eps = size * num_of_threads; // this number of end points
	int patch=0, ep=0;
	int number_of_patches = input.xpatches * input.ypatches * input.zpatches;
	int patches_per_ep = number_of_patches / num_of_eps;

	g_patch_ep.resize(input.xpatches * input.ypatches * input.zpatches);

	for(int i=0; i<input.zpatches; i++)
	for(int j=0; j<input.ypatches; j++)
	for(int k=0; k<input.xpatches; k++){
		g_patch_ep[patch++] = ep;
		if(patch % patches_per_ep == 0) ep++;
	}


	//loop over dimensions and find out number of "superpatches" used to find out comm directions
	//Each superpatch is a combined patch of all patches assigned to an EP. Thus mimics one patch per EP assignment.
	//CAUTION: can be changed with the new logic of dividing threads in three dimensions.

	g_superpatches[0] = input.xpatches, g_superpatches[1] = input.ypatches, g_superpatches[2] = input.zpatches;

	for(int i=0; i<3; i++){
		if(g_superpatches[i] >= patches_per_ep){ //patches fit within the given dimension. recompute and break
			if(g_superpatches[i] % patches_per_ep != 0){ //patch dim should be multiple of patches_per_ep
				printf("Part 1: number of patches per EP should be aligned with patch dimensions at %s:%d\n", __FILE__, __LINE__);exit(1);
			}
			g_superpatches[i] = g_superpatches[i] / patches_per_ep;
			patches_per_ep = 1;
			break;
		}
		else{//there can be multiple rows or planes assigned to one EP
			if(patches_per_ep % g_superpatches[i] != 0){ //now patches_per_ep should be multiple of patch_dim
				printf("Part 2: number of patches per EP should be aligned with patch dimensions at %s:%d\n", __FILE__, __LINE__);exit(1);
			}
			patches_per_ep = patches_per_ep / g_superpatches[i];
			g_superpatches[i] = 1; //current dimension will be 1 because no division across this dimension
		}
	}

	if(patches_per_ep !=1 || num_of_eps != g_superpatches[0]*g_superpatches[1]*g_superpatches[2]){
		printf("Part 3: Error in superpatch generation logic at %s:%d\n", __FILE__, __LINE__);exit(1);
	}

	//iterate over eps and map eps to superpatces.
	g_ep_superpatch.resize(num_of_eps);
	for(int i=0; i<num_of_eps; i++){//num_of_eps = num_superpatches
		int base_patch = i * patches_per_ep;
		int ep = g_patch_ep[base_patch];
		g_ep_superpatch[ep] = i; //this gives mapping of rank to super-patch

	}


	/*MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	if(rank==0)
		for(int i=0; i<g_patch_ep.size(); i++)
			printf("patch assignment: %d %d\n", i, g_patch_ep[i]);*/
}

//Quick  and really really dirty. Try improving later. should be called after assignPatchToEP.
void getMyPatches( std::vector<int>& my_patches, int my_rank)
{
	for(int i=0; i<g_patch_ep.size(); i++){
		if(g_patch_ep[i] == my_rank)	my_patches.push_back(i);
	}
}

/*
1. Loop over my patches. For every patch:
2. Loop over (-1, -1, -1) to (1, 1, 1). Calculate offset for every direction (i, j, k) and add it to the current patch to get neighboring patch
3. Find proc of neighbor patch. Add it if its not same as self.
*/
/*void findNeighbors(std::vector<RankDir> & neighbors, std::vector<int>& my_patches, int x_patches, int y_patches, int z_patches, int rank){

	for(int p : my_patches){	
		int iid = p / (y_patches*x_patches);
		int jid = (p % (y_patches*x_patches)) / x_patches;
		int kid = p % x_patches;

		for(int i=iid-1; i<iid+2; i++)
		for(int j=jid-1; j<jid+2; j++)
		for(int k=kid-1; k<kid+2; k++){
			if(i > -1 && i < z_patches && j > -1 && j < y_patches && k > -1 && k < x_patches){
				int neighbor = i * y_patches * x_patches + j * x_patches + k;
				int proc = g_patch_ep[neighbor];
				if(proc != rank){
					//printf("pushing neighbours %d %d %d %d %d\n",k-kid, j-jid, i-iid, rank, proc);
					neighbors.push_back(RankDir(k-kid, j-jid, i-iid, rank, proc)); //for send
					neighbors.push_back(RankDir(k-kid, j-jid, i-iid, proc, rank)); //for recv
					//if(rank==4) 	printf("neighbors %d %d %d %d %d\n", neighbor, i, j, k, p);
				}
			}
		}		
	}
}*/

void hypre_solve(const char * exp_type, xmlInput input)
{

	//--------------------------------------------------------- init ----------------------------------------------------------------------------------------------

	int number_of_patches = input.xpatches * input.ypatches * input.zpatches;

	
	HYPRE_Init(argc, argv);

	int rank, size;
	hypre_MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	hypre_MPI_Comm_size(MPI_COMM_WORLD, &size);

	if(number_of_patches % size != 0){
		printf("Ensure total number of patches (patches cube) is divisible number of ranks at %s:%d. exiting\n", __FILE__, __LINE__);
		exit(1);
	}

	int num_cells = number_of_patches*input.patch_size*input.patch_size*input.patch_size;

	thread_local extern double _hypre_comm_time;
	//----------------------------------------------------------------------------------------------------------------------------------------------------------


	//----------------------------------------------- patch set up----------------------------------------------------------------------------------------------

	std::vector<int> my_patches;
	getMyPatches( my_patches, rank );

	//std::vector<RankDir> neighbors;
	//findNeighbors(neighbors, my_patches, input.xpatches, input.ypatches, input.zpatches, rank); //assuming EP.

	createCommMap(g_ep_superpatch.data(), g_superpatches, input.xthreads, input.ythreads, input.zthreads);	//in hypre_mpi_ep_helper.h

	int patches_per_rank = my_patches.size();
	int x_dim = input.patch_size, y_dim = input.patch_size, z_dim = input.patch_size;

	std::vector<IntVector> low(patches_per_rank), high(patches_per_rank);	//store boundaries of every patch

	for(int i=0; i<patches_per_rank; i++)
	{
		//patch id based on local patch number (i) + starting patch assigned to this rank.
		int patch_id = my_patches[i];

		// convert patch_id into 3d patch co-cordinates. Multiply by patch_size to get starting cell of patch.
		low[i].value[0] = x_dim * (patch_id % input.xpatches);
		low[i].value[1] = y_dim * ((patch_id / input.xpatches) % input.ypatches);
		low[i].value[2] = z_dim * ((patch_id / input.xpatches) / input.ypatches);

		// add patch_size to low to get end cell of patch. subtract 1 as hypre uses inclusive boundaries... as per Uintah code
		high[i].value[0] = low[i].value[0] + x_dim - 1; //including high cell. hypre needs so.. i guess
		high[i].value[1] = low[i].value[1] + y_dim - 1;
		high[i].value[2] = low[i].value[2] + z_dim - 1;
		//printf("%d/%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", rank, size, patch_id, low[i].value[0], low[i].value[1], low[i].value[2], high[i].value[0], high[i].value[1], high[i].value[2]);
	}

	//All patches will have same values. Hence just create 1 patch for A and 1 for B. Pass same values again and again for every patch.
	//ViewDouble X("X", x_dim * y_dim * z_dim), B("B", x_dim * y_dim * z_dim);
	//ViewStencil4 A("A", x_dim * y_dim * z_dim);
	Stencil4 A[x_dim * y_dim * z_dim];
	double X[x_dim * y_dim * z_dim], B[x_dim * y_dim * z_dim];

//	Kokkos::parallel_for(Kokkos::RangePolicy<KernelSpace>(0, x_dim * y_dim * z_dim), KOKKOS_LAMBDA(int i){
	for(int i=0; i<x_dim * y_dim * z_dim; i++){
			A[i].p = 6; A[i].n=-1; A[i].e=-1; A[i].t=-1;
			B[i] = 1;
			X[i] = 0;

	}
//	});

	//----------------------------------------------------------------------------------------------------------------------------------------------------------



	//----------------------------------------------------- hypre -----------------------------------------------------------------------------------------------

	extern thread_local bool do_setup;

	HYPRE_StructSolver * solver = new HYPRE_StructSolver;
	HYPRE_StructSolver * precond_solver = new HYPRE_StructSolver;
	HYPRE_StructMatrix * HA = new HYPRE_StructMatrix;
	HYPRE_StructVector * HB = new HYPRE_StructVector;
	HYPRE_StructVector * HX = new HYPRE_StructVector;
	HYPRE_StructGrid grid;
	int periodic[]={0,0,0};
	HYPRE_StructStencil stencil;
	int offsets[4][3] = {{0,0,0},
			{-1,0,0},
			{0,-1,0},
			{0,0,-1}};
	bool HB_created = false;
	bool HA_created = false;
	bool HX_created = false;

	for(int timestep=0; timestep<6; timestep++)
	{
		_hypre_comm_time = 0.0;
		auto start = std::chrono::system_clock::now();

		if(do_setup)
		{
			// Setup grid
			HYPRE_StructGridCreate(MPI_COMM_WORLD, 3, &grid);
			for(int i=0; i<patches_per_rank; i++)
				HYPRE_StructGridSetExtents(grid, low[i].value, high[i].value);

			HYPRE_StructGridSetPeriodic(grid, periodic);	// No Periodic boundaries

			HYPRE_StructGridAssemble(grid);	// Assemble the grid

			HYPRE_StructStencilCreate(3, 4, &stencil);	// Create the stencil
			for(int i=0;i<4;i++)
				HYPRE_StructStencilSetElement(stencil, i, offsets[i]);

			// Create the matrix
			if(HA_created)	//just in case if we have to loop over
				HYPRE_StructMatrixDestroy(*HA);
			HYPRE_StructMatrixCreate(MPI_COMM_WORLD, grid, stencil, HA);
			HA_created = true;
			HYPRE_StructMatrixSetSymmetric(*HA, true);
			int ghost[] = {1,1,1,1,1,1};
			HYPRE_StructMatrixSetNumGhost(*HA, ghost);
			HYPRE_StructMatrixInitialize(*HA);

			// Create the RHS
			if(HB_created)	//just in case if we have to loop over
				HYPRE_StructVectorDestroy(*HB);
			HYPRE_StructVectorCreate(MPI_COMM_WORLD, grid, HB);
			HB_created = true;
			HYPRE_StructVectorInitialize(*HB);

			// setup the coefficient matrix
			int stencil_indices[] = {0,1,2,3};

			for(int i=0; i<patches_per_rank; i++){
				double *values = reinterpret_cast<double *>(A);
				HYPRE_StructMatrixSetBoxValues(*HA, low[i].value, high[i].value,
						4, stencil_indices,
						values);
			}

			HYPRE_StructMatrixAssemble(*HA);
		}//if(do_setup)

		//set up RHS

		for(int i=0; i<patches_per_rank; i++)
			HYPRE_StructVectorSetBoxValues(*HB, low[i].value, high[i].value, B);

		if(do_setup)
			HYPRE_StructVectorAssemble(*HB);



		if(do_setup)		// Create the solution vector
		{
			if(HX_created)	//just in case if we have to loop over
				HYPRE_StructVectorDestroy(*HX);
			HYPRE_StructVectorCreate(MPI_COMM_WORLD, grid, HX);
			HX_created=true;
			HYPRE_StructVectorInitialize(*HX);
			HYPRE_StructVectorAssemble(*HX);
		}

		//solve the system
		bool solver_created = false;

		if (do_setup)
		{
			if(solver_created) //just in case if we have to loop over
				HYPRE_StructPCGDestroy(*solver);
			HYPRE_StructPCGCreate(MPI_COMM_WORLD,solver);
			solver_created = true;
		}

		HYPRE_StructPCGSetMaxIter(*solver, 100);
		HYPRE_StructPCGSetTol(*solver, tolerance);
		HYPRE_StructPCGSetTwoNorm(*solver,  1);
		HYPRE_StructPCGSetRelChange(*solver,  0);
		HYPRE_StructPCGSetLogging(*solver,  0);

		HYPRE_PtrToStructSolverFcn precond;
		HYPRE_PtrToStructSolverFcn precond_setup;
		auto solve_only_start = std::chrono::system_clock::now();
		if (do_setup)
		{
			HYPRE_StructPFMGCreate        (MPI_COMM_WORLD,    precond_solver);
			HYPRE_StructPFMGSetMaxIter    (*precond_solver,   1);
			HYPRE_StructPFMGSetTol        (*precond_solver,   0.0);
			HYPRE_StructPFMGSetZeroGuess  (*precond_solver);

			/* weighted Jacobi = 1; red-black GS = 2 */
			HYPRE_StructPFMGSetRelaxType   (*precond_solver,  1);
			HYPRE_StructPFMGSetNumPreRelax (*precond_solver,  1);
			HYPRE_StructPFMGSetNumPostRelax(*precond_solver,  1);
			HYPRE_StructPFMGSetSkipRelax   (*precond_solver,  0);
			HYPRE_StructPFMGSetLogging     (*precond_solver,  0);

			precond = HYPRE_StructPFMGSolve;
			precond_setup = HYPRE_StructPFMGSetup;
			HYPRE_StructPCGSetPrecond(*solver, precond,precond_setup, *precond_solver);

			HYPRE_StructPCGSetup(*solver, *HA, *HB, *HX);

		}
		do_setup = false;
		//printf("%d do_setup false\n", omp_get_thread_num());
		//sleep(2);
		HYPRE_StructPCGSolve(*solver, *HA, *HB, *HX);	//main solve

		int num_iterations;
		HYPRE_StructPCGGetNumIterations(*solver, &num_iterations);

		double final_res_norm;
		HYPRE_StructPCGGetFinalRelativeResidualNorm(*solver,&final_res_norm);

		auto solve_only_end = std::chrono::system_clock::now();

		if(rank ==0 && (final_res_norm > tolerance || std::isfinite(final_res_norm) == 0))
		{
			std::cout << "HypreSolver not converged in " << num_iterations << " iterations, final residual= " << final_res_norm << " at " <<  __FILE__  << ":" << __LINE__ << "\n";
			exit(1);
		}

		for(int i=0; i<patches_per_rank; i++)
			HYPRE_StructVectorGetBoxValues(*HX, low[i].value, high[i].value, X);

		auto end = std::chrono::system_clock::now();
		std::chrono::duration<double> comp_time = end - start;
		std::chrono::duration<double> solve_time = solve_only_end - solve_only_start;

		double t_comp_time, avg_comp_time=0.0, t_solve_time, avg_solve_time=0.0, avg_comm_time=0.0;
		t_comp_time = comp_time.count();
		t_solve_time = solve_time.count();

		hypre_MPI_Allreduce(&_hypre_comm_time, &avg_comm_time, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);			
		hypre_MPI_Allreduce(&t_comp_time, &avg_comp_time, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
		hypre_MPI_Allreduce(&t_solve_time, &avg_solve_time, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

		avg_comm_time /= size;
		avg_comp_time /= size;
		avg_solve_time /= size;

		std::cout.precision(2);
		//exp_type			//size			num_cells   		patch_size   		x_patches    		y_patches   		z_patches
		//timestep			 total_time				solve_only_time			avg_comm_time				num_iterations			final_res_norm
		if(rank==0 && omp_get_thread_num()==0)
			std::cout <<
			exp_type << "\t" << size << "\t" << num_cells << "\t" << input.patch_size << "\t" << input.xpatches << "\t" << input.ypatches << "\t" << input.zpatches << "\t" <<
			timestep << "\t" << avg_comp_time << "\t" << avg_solve_time << "\t" << avg_comm_time << "\t" << num_iterations << "\t" << final_res_norm << 
			"\n" ;

		//verifyX(X, x_dim * y_dim * z_dim);
	}//for(int timestep=0; timestep<11; timestep++)


	//----------------------------------------------------------------------------------------------------------------------------------------------------------

	//clean up
	HYPRE_StructStencilDestroy(stencil);
	HYPRE_StructGridDestroy(grid);
	hypre_EndTiming (tHypreAll_);

	fflush(stdout);
	HYPRE_Finalize();
}	//end of hypre_solve


int main(int argc1, char **argv1)
{
	if(argc1 != 3){
		printf("Enter arguments id_string and input file name at %s:%d. exiting\n", __FILE__, __LINE__);
		exit(1);
	}
	argc = argc1;
	argv = argv1;
	
	const char * exp_type = argv[1];	//cpu / gpu / gpu-mps / gpu-superpatch. Only to print in output.

//	Kokkos::initialize(argc, argv1);
	{
		int required=MPI_THREAD_MULTIPLE, provided;
		MPI_Init_thread(&argc, &argv, required, &provided);
		
		//Kokkos::OpenMP::partition_master( hypre_solve, omp_get_num_threads(), 1);
		
//		Kokkos::parallel_for(Kokkos::RangePolicy<Kokkos::OpenMP>(0, partitions), [&](int i){
//			hypre_solve(partitions, 1);
//		});

		int rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		xmlInput input = parseInput(argv[2], rank);

		if(input.xthreads>0 && input.ythreads>0 && input.zthreads>0){	//override #threads
			omp_set_num_threads(input.xthreads*input.ythreads*input.zthreads);
 		    if(rank ==0) printf("number of threads %d, %d, %d\n", input.xthreads, input.ythreads, input.zthreads);
		}

		int threads = omp_get_max_threads();

		if(rank ==0) printf("Number of threads %d\n", threads);

		assignPatchToEP(input, size, threads); //assuming EP.

		hypre_set_num_threads(threads, omp_get_thread_num);
		//cudaProfilerStart();
#pragma omp parallel
		{
			/*int cpu_affinity = get_affinity();
			int device_id=-1;
			cudaGetDevice(&device_id);
			cudaDeviceProp deviceProp;
			cudaGetDeviceProperties(&deviceProp, device_id);
			printf("Device id: %d,  Device PCI Domain ID / Bus ID / location ID:   %d / %d / %d, cpu_affinity: %d\n", 
					device_id, deviceProp.pciDomainID, deviceProp.pciBusID, deviceProp.pciDeviceID, cpu_affinity);*/
			
			hypre_solve(exp_type, input);
		}
		//cudaProfilerStop();
		
		MPI_Finalize();			


	}
//	Kokkos::finalize();
	return 0;

}
