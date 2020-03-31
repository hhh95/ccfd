/*
 * mesh.c
 *
 * Created: Tue 24 Mar 2020 12:45:57 PM CET
 * Author : hhh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "main.h"
#include "mesh.h"
#include "readInTools.h"
#include "output.h"
#include "timeDiscretization.h"
#include "memTools.h"

/* extern variables */
char parameterFile[STRLEN],
     strMeshFormat[STRLEN],
     strMeshFile[STRLEN],
     strIniCondFile[STRLEN];

cartMesh_t cartMesh;
char gridFile[STRLEN];

int meshType;
int meshFormat;

long nNodes;

long nElems;
long nTrias;
long nQuads;

long nSides;
long nBCsides;
long nInnerSides;

double totalArea_q;
double xMin, xMax;
double yMin, yMax;
double dxRef;

elem_t **elem;
side_t **side;
side_t **BCside;

elem_t *firstElem;
node_t *firstNode;
side_t *firstSide;
sidePtr_t *firstBCside;

typedef struct sideList_t sideList_t;
struct sideList_t {
	long node[2];
	bool BC;
	side_t *side;
	bool isRotated;
};

/*
 * compute required vectors for reconstruction
 */
void createReconstructionInfo(elem_t *aElem)
{
	double r11, r12, r22;
	r11 = r12 = r22 = 0.0;

	side_t *aSide = aElem->firstSide;
	while (aSide) {
		aSide->baryBaryVec[X] = aSide->GP[X] - aSide->connection->GP[X];
		aSide->baryBaryVec[Y] = aSide->GP[Y] - aSide->connection->GP[Y];

		aSide->baryBaryDist = sqrt(aSide->baryBaryVec[X] * aSide->baryBaryVec[X] +
				           aSide->baryBaryVec[Y] * aSide->baryBaryVec[Y]);

		r11 += aSide->baryBaryVec[X] * aSide->baryBaryVec[X];
		r12 += aSide->baryBaryVec[X] * aSide->baryBaryVec[Y];
		r22 += aSide->baryBaryVec[Y] * aSide->baryBaryVec[Y];

		aSide = aSide->nextElemSide;
	}

	r11 = sqrt(r11);
	r12 = r12 / r11;
	r22 = sqrt(r22 - r12 * r12);

	aSide = aElem->firstSide;
	while (aSide) {
		double alpha1 = aSide->baryBaryVec[X] / (r11 * r11);
		double alpha2 = (aSide->baryBaryVec[Y] - r12 / r11 *
				aSide->baryBaryVec[X]) / (r22 * r22);

		aSide->w[X] = alpha1 - r12 / r11 * alpha2;
		aSide->w[Y] = alpha2;

		aSide = aSide->nextElemSide;
	}

	/* gaussian integration points and weights for volume integrals */
	switch (aElem->elemType) {
	case 3:
		aElem->nGP = 3;
		aElem->wGP = malloc(aElem->nGP * sizeof(double));
		aElem->xGP = dyn2DdblArray(aElem->nGP, NDIM);

		aSide = aElem->firstSide;
		for (int iGP = 0; iGP < aElem->nGP; ++iGP) {
			aElem->xGP[iGP][X] = aElem->bary[X] + aSide->GP[X];
			aElem->xGP[iGP][Y] = aElem->bary[Y] + aSide->GP[Y];

			aElem->wGP[iGP] = aElem->area / 3.0;

			aSide = aSide->nextElemSide;
		}
		break;
	case 4:
		aElem->nGP = 5;
		aElem->wGP = malloc(aElem->nGP * sizeof(double));
		aElem->xGP = dyn2DdblArray(aElem->nGP, NDIM);

		aSide = aElem->firstSide;
		for (int iGP = 0; iGP < aElem->nGP - 1; ++iGP) {
			aElem->xGP[iGP][X] = aElem->bary[X] + aSide->GP[X];
			aElem->xGP[iGP][Y] = aElem->bary[Y] + aSide->GP[Y];

			aElem->wGP[iGP] = aElem->area / 6.0;

			aSide = aSide->nextElemSide;
		}
		aElem->xGP[aElem->nGP - 1][X] = (aElem->node[0]->x[X]
				+ aElem->node[2]->x[X]) / 2.0;
		aElem->xGP[aElem->nGP - 1][Y] = (aElem->node[0]->x[Y]
				+ aElem->node[2]->x[Y]) / 2.0;
		aElem->wGP[aElem->nGP - 1] = aElem->area / 3.0;
		break;
	default:
		printf("| ERROR in createReconstructionInfo:\n");
		printf("| No quadrature rule for elements other than triangles or quadrilaterals\n");
		exit(1);
	}
}

/*
 * create connection for periodic BCs
 */
void connectPeriodicBC(void)
{
	isPeriodic = false;

	sidePtr_t *aBCside = firstBCside;
	double aGPpos[2], sGPpos[2];
	bool isConnected;
	while (aBCside) {
		side_t *aSide = aBCside->side;
		if ((aSide->BC->BCtype == PERIODIC) &&
		    ((aSide->BC->BCid % 10) == 1)) {
			isPeriodic = true;
			isConnected = false;

			aGPpos[X] = aSide->GP[X] + aSide->elem->bary[X];
			aGPpos[Y] = aSide->GP[Y] + aSide->elem->bary[Y];

			int nPeriodicBC = aSide->BC->BCid / 10;

			sidePtr_t *sBCside = firstBCside;
			while (sBCside) {
				side_t *sSide = sBCside->side;
				if ((sSide->BC->BCtype == PERIODIC) &&
				    ((sSide->BC->BCid % 10) == 2) &&
				    ((sSide->BC->BCid / 10) == nPeriodicBC)) {
					sGPpos[X] = sSide->GP[X] + sSide->elem->bary[X];
					sGPpos[Y] = sSide->GP[Y] + sSide->elem->bary[Y];
					if ((fabs(aGPpos[X] + aSide->BC->connection[X] - sGPpos[X]) +
					     fabs(aGPpos[Y] + aSide->BC->connection[Y] - sGPpos[Y])) <= REALTOL) {
						isConnected = true;

						side_t *urSide = aSide->connection;
						side_t *targetSide = sSide->connection;
						urSide->connection = targetSide;
						targetSide->connection = urSide;

						/* reorganise lists: target side has to be removed */
						nSides--;
						if (firstSide == targetSide) {
							firstSide = firstSide->next;
						} else {
							side_t *sSide = firstSide;
							while (sSide) {
								if (sSide->next == targetSide) {
									sSide->next = targetSide->next;
									break;
								}
								sSide = sSide->next;
							}
						}
						break;
					}
				}

				sBCside = sBCside->next;
			}

			if (!isConnected) {
				printf("| ERROR in connectPeriodicBC: No connection found\n");
				printf("| Side GP was: %g %g\n", aGPpos[X], aGPpos[Y]);
				exit(1);
			}
		}

		aBCside = aBCside->next;
	}
}

/*
 * create side info: normal vector, side length, ghost cells, ...
 */
void createSideInfo(side_t *aSide)
{
	aSide->n[X] = aSide->node[1]->x[Y] - aSide->node[0]->x[Y];
	aSide->n[Y] = aSide->node[0]->x[X] - aSide->node[1]->x[X];

	aSide->len = sqrt(aSide->n[X] * aSide->n[X] + aSide->n[Y] * aSide->n[Y]);

	aSide->n[X] /= aSide->len;
	aSide->n[Y] /= aSide->len;

	aSide->GP[X] = (aSide->node[0]->x[X] + aSide->node[1]->x[X]) / 2.0
		- aSide->elem->bary[X];
	aSide->GP[Y] = (aSide->node[0]->x[Y] + aSide->node[1]->x[Y]) / 2.0
		- aSide->elem->bary[Y];

	/* compute dot-product of vector from barycenter to side midpoint with
	 * normal vector, if it is less than zero, the direction must be
	 * switched */
	if ((aSide->n[X] * aSide->GP[X] + aSide->n[Y] * aSide->GP[Y]) < 0.0) {
		aSide->n[X] *= -1.0;
		aSide->n[Y] *= -1.0;
	}

	/* compute barycenter of ghost cell in case it is a boundary side */
	if (aSide->connection->BC) {
		double tmp = 2.0 * fabs(aSide->GP[X] * aSide->n[X]
				      + aSide->GP[Y] * aSide->n[Y]);
		aSide->connection->elem->bary[X] = aSide->elem->bary[X]
			+ tmp * aSide->n[X];
		aSide->connection->elem->bary[Y] = aSide->elem->bary[Y]
			+ tmp * aSide->n[Y];
	}

	aSide->connection->GP[X] = aSide->GP[X] - aSide->connection->elem->bary[X];
	aSide->connection->GP[Y] = aSide->GP[Y] - aSide->connection->elem->bary[Y];
	aSide->connection->n[X] = aSide->n[X];
	aSide->connection->n[Y] = aSide->n[Y];
	aSide->connection->len = aSide->len;
}

/*
 * compute the cell specific values: barycenter, area, projection length of cell
 */
void createElemInfo(elem_t *aElem)
{
	/* compute barycenter */
	aElem->bary[X] = aElem->bary[Y] = 0.0;
	for (int i = 0; i < aElem->elemType; ++i) {
		for (int j = 0; j < NDIM; ++j) {
			aElem->bary[j] += aElem->node[i]->x[j] / aElem->elemType;
		}
	}

	/* element area and its inverse */
	double vec[2], n[4][2], len[4];
	for (int i = 0; i < aElem->elemType - 1; ++i) {
		vec[X] = aElem->node[i + 1]->x[X] - aElem->node[i]->x[X];
		vec[Y] = aElem->node[i + 1]->x[Y] - aElem->node[i]->x[Y];

		len[i] = sqrt(vec[X] * vec[X] + vec[Y] * vec[Y]);

		n[i][X] = aElem->node[i + 1]->x[Y] - aElem->node[i]->x[Y];
		n[i][Y] = aElem->node[i]->x[X] - aElem->node[i + 1]->x[X];

		double tmp = sqrt(n[i][X] * n[i][X] + n[i][Y] * n[i][Y]);
		n[i][X] /= tmp;
		n[i][Y] /= tmp;
	}

	vec[X] = aElem->node[0]->x[X] - aElem->node[aElem->elemType - 1]->x[X];
	vec[Y] = aElem->node[0]->x[Y] - aElem->node[aElem->elemType - 1]->x[Y];

	len[aElem->elemType - 1] = sqrt(vec[X] * vec[X] + vec[Y] * vec[Y]);

	n[aElem->elemType - 1][X] =
		aElem->node[0]->x[Y] - aElem->node[aElem->elemType - 1]->x[Y];
	n[aElem->elemType - 1][Y] =
		aElem->node[aElem->elemType - 1]->x[X] - aElem->node[0]->x[X];

	double tmp = sqrt(n[aElem->elemType - 1][X] * n[aElem->elemType - 1][X]
			+ n[aElem->elemType - 1][Y] * n[aElem->elemType - 1][Y]);
	n[aElem->elemType - 1][X] /= tmp;
	n[aElem->elemType - 1][Y] /= tmp;

	aElem->area = len[0] * len[1] * fabs(n[0][X]*n[1][Y] - n[1][X]*n[0][Y]);
	if (aElem->elemType == 3) {
		aElem->area /= 2;
	}
	aElem->areaq = 1.0 / aElem->area;

	if (totalArea_q != 0.0) {
		totalArea_q = 1.0 / (1.0 / totalArea_q + aElem->area);
	} else {
		totalArea_q = aElem->areaq;
	}

	/* projection of cell onto x- and y-axis */
	aElem->sx = aElem->sy = 0;
	for (int i = 0; i < aElem->elemType; ++i) {
		aElem->sx += fabs(n[i][X]) * len[i] / 2.0;
		aElem->sy += fabs(n[i][Y]) * len[i] / 2.0;
	}
}

/*
 * compare two elements of sideList
 */
int compare(const void *a, const void *b)
{
	sideList_t *A = (sideList_t *)a;
	sideList_t *B = (sideList_t *)b;
	if (A->node[0] == B->node[0]) {
		if (A->node[1] == B->node[1]) {
			return A->BC - B->BC;
		} else {
			return A->node[1] - B->node[1];
		}
	} else {
		return A->node[0] - B->node[0];
	}
}

/*
 * create a cartesian mesh
 */
void createCartMesh(
	double ***vertex, long *nVertices, long ***BCedge, long *nBCedges, long ***quad)
{
	/* set up necessary variables */
	long iMax = cartMesh.iMax;
	long jMax = cartMesh.jMax;

	*nVertices = (iMax + 1) * (jMax + 1);
	nQuads = iMax * jMax;
	*nBCedges = 2 * (iMax + jMax);

	double dx = (xMax - xMin) / iMax;
	double dy = (yMax - yMin) / jMax;

	*quad = dyn2DintArray(nQuads, 5);
	*BCedge = dyn2DintArray(*nBCedges, 3);
	*vertex = dyn2DdblArray(*nVertices, 2);

	/* build vertices */
	double y = yMin, x;
	long k = 0;
	for (long j = 0; j < jMax + 1; ++j) {
		x = xMin;
		for (long i = 0; i < iMax + 1; ++i) {
			(*vertex)[k][X] = x;
			(*vertex)[k][Y] = y;
			//printf("%ld %g %g\n", k, (*vertex)[k][X], (*vertex)[k][Y]);
			k++;
			x += dx;
		}
		y += dy;
	}

	/* build elements */
	k = 0;
	for (long j = 0; j < jMax; ++j) {
		for (long i = 0; i < iMax; ++i) {
			(*quad)[k][0] = k + j;
			(*quad)[k][1] = (*quad)[k][0] + 1;
			(*quad)[k][2] = (*quad)[k][1] + iMax + 1;
			(*quad)[k][3] = (*quad)[k][2] - 1;
			(*quad)[k][4] = 1;
			//printf("%ld %ld %ld %ld %ld %ld\n", k, (*quad)[k][0], (*quad)[k][1], (*quad)[k][2], (*quad)[k][3], (*quad)[k][4]);
			k++;
		}
	}

	/* build BCedges */
	k = 0;
	/* bottom side */
	printf("| Bottom Side:\n");
	printf("|   # of BCs: %d\n", cartMesh.nBC[BOTTOM]);
	for (int iBC = 0; iBC < cartMesh.nBC[BOTTOM]; ++iBC) {
		printf("|   BC Type : %d\n", cartMesh.BCtype[BOTTOM][iBC]);
		printf("|   BC Range: %d -- %d\n", cartMesh.BCrange[BOTTOM][iBC][0], cartMesh.BCrange[BOTTOM][iBC][1]);
		for (int i = cartMesh.BCrange[BOTTOM][iBC][0] - 1; i < cartMesh.BCrange[BOTTOM][iBC][1]; ++i) {
			(*BCedge)[k][0] = i;
			(*BCedge)[k][1] = i + 1;
			(*BCedge)[k][2] = cartMesh.BCtype[BOTTOM][iBC];
			//printf("%ld %ld %ld %ld\n", k, (*BCedge)[k][0], (*BCedge)[k][1], (*BCedge)[k][2]);
			k++;
		}
	}
	/* right side */
	printf("| Right Side:\n");
	printf("|   # of BCs: %d\n", cartMesh.nBC[RIGHT]);
	for (int iBC = 0; iBC < cartMesh.nBC[RIGHT]; ++iBC) {
		printf("|   BC Type : %d\n", cartMesh.BCtype[RIGHT][iBC]);
		printf("|   BC Range: %d -- %d\n", cartMesh.BCrange[RIGHT][iBC][0], cartMesh.BCrange[RIGHT][iBC][1]);
		for (int j = cartMesh.BCrange[RIGHT][iBC][0] - 1; j < cartMesh.BCrange[RIGHT][iBC][1]; ++j) {
			(*BCedge)[k][0] = (iMax + 1) * (j + 1) - 1;
			(*BCedge)[k][1] = (iMax + 1) * (j + 2) - 1;
			(*BCedge)[k][2] = cartMesh.BCtype[RIGHT][iBC];
			//printf("%ld %ld %ld %ld\n", k, (*BCedge)[k][0], (*BCedge)[k][1], (*BCedge)[k][2]);
			k++;
		}
	}
	/* top side */
	printf("| Top Side:\n");
	printf("|   # of BCs: %d\n", cartMesh.nBC[TOP]);
	for (int iBC = 0; iBC < cartMesh.nBC[TOP]; ++iBC) {
		printf("|   BC Type : %d\n", cartMesh.BCtype[TOP][iBC]);
		printf("|   BC Range: %d -- %d\n", cartMesh.BCrange[TOP][iBC][0], cartMesh.BCrange[TOP][iBC][1]);
		for (int i = cartMesh.BCrange[TOP][iBC][0] - 1; i < cartMesh.BCrange[TOP][iBC][1]; ++i) {
			(*BCedge)[k][0] = (iMax + 1) * jMax + i;
			(*BCedge)[k][1] = (*BCedge)[k][0] + 1;
			(*BCedge)[k][2] = cartMesh.BCtype[TOP][iBC];
			//printf("%ld %ld %ld %ld\n", k, (*BCedge)[k][0], (*BCedge)[k][1], (*BCedge)[k][2]);
			k++;
		}
	}
	/* left side */
	printf("| Left Side:\n");
	printf("|   # of BCs: %d\n", cartMesh.nBC[LEFT]);
	for (int iBC = 0; iBC < cartMesh.nBC[LEFT]; ++iBC) {
		printf("|   BC Type : %d\n", cartMesh.BCtype[LEFT][iBC]);
		printf("|   BC Range: %d -- %d\n", cartMesh.BCrange[LEFT][iBC][0], cartMesh.BCrange[LEFT][iBC][1]);
		for (int j = cartMesh.BCrange[LEFT][iBC][0] - 1; j < cartMesh.BCrange[LEFT][iBC][1]; ++j) {
			(*BCedge)[k][0] = j * (iMax + 1);
			(*BCedge)[k][1] = (*BCedge)[k][0] + iMax + 1;
			(*BCedge)[k][2] = cartMesh.BCtype[LEFT][iBC];
			//printf("%ld %ld %ld %ld\n", k, (*BCedge)[k][0], (*BCedge)[k][1], (*BCedge)[k][2]);
			k++;
		}
	}
}

/*
 * create a cartesian, structured mesh
 * read in of all supported mesh types:
 * *.msh, *.emc2, *.cgns
 */
void createMesh(void)
{
	nTrias = nQuads = 0;

	/* create cartesian mesh or read unstructured mesh from file */
	double **vertex;
	long **tria, **quad, **BCedge, *zoneConnect;
	tria = quad = BCedge = NULL;
	long nVertices, nBCedges; //, nZones;
	switch (meshType) {
	case CARTESIAN:
		createCartMesh(&vertex, &nVertices, &BCedge, &nBCedges, &quad);
		zoneConnect = calloc(nVertices, sizeof(long));
		break;
	case UNSTRUCTURED:
		// TODO: read in unstructured mesh
		break;
	}

	/* generate mesh information */
	nElems = nTrias + nQuads;
	nInnerSides = (3 * nTrias + 4 * nQuads - nBCedges) / 2;
	nBCsides = nBCedges;
	nSides = nInnerSides + nBCedges;
	nNodes = 0;

	/* create nodes */
	node_t *vertexPtr[nVertices];
	for (long iNode = 0; iNode < nVertices; ++iNode) {
		vertexPtr[iNode] = NULL;
	}

	xMin = vertex[0][X];
	xMax = vertex[0][X];
	yMin = vertex[0][Y];
	yMax = vertex[0][Y];

	firstNode = NULL;
	for (long iNode = nVertices - 1; iNode >= 0; --iNode) {
		if (zoneConnect[iNode] == 0) {
			node_t *aNode = malloc(sizeof(node_t));
			aNode->id = iNode;
			aNode->x[X] = vertex[iNode][X];
			aNode->x[Y] = vertex[iNode][Y];

			xMin = fmin(aNode->x[X], xMin);
			xMax = fmax(aNode->x[X], xMax);
			yMin = fmin(aNode->x[Y], yMin);
			yMax = fmax(aNode->x[Y], yMax);

			aNode->next = firstNode;
			firstNode = aNode;

			vertexPtr[iNode] = aNode;

			nNodes++;
		} else {
			if (vertexPtr[zoneConnect[iNode]]) {
				/* already built */
				vertexPtr[iNode] = vertexPtr[zoneConnect[iNode]];
			} else {
				node_t *aNode = malloc(sizeof(node_t));
				aNode->id = iNode;
				aNode->x[X] = vertex[iNode][X];
				aNode->x[Y] = vertex[iNode][Y];

				xMin = fmin(aNode->x[X], xMin);
				xMax = fmax(aNode->x[X], xMax);
				yMin = fmin(aNode->x[Y], yMin);
				yMax = fmax(aNode->x[Y], yMax);

				aNode->next = firstNode;
				firstNode = aNode;

				vertexPtr[iNode] = aNode;
				vertexPtr[zoneConnect[iNode]] = vertexPtr[iNode];

				nNodes++;
			}
		}
	}
	free(vertex);
	free(zoneConnect);

	/* elements and side pointers */
	sideList_t *sideList = malloc(2 * nSides * sizeof(sideList_t));

	firstElem = NULL;
	long iElem = 0, iSidePtr = 0;

	/* loop over all triangles */
	elem_t *prevElem = NULL;
	for (long iTria = 0; iTria < nTrias; ++iTria) {
		elem_t *aElem = malloc(sizeof(elem_t));
		aElem->id = iElem++;
		aElem->domain = tria[iTria][3];
		aElem->elemType = 3;

		if (!firstElem) {
			firstElem = aElem;
			prevElem = aElem;
		} else {
			prevElem->next = aElem;
			prevElem = aElem;
		}
		aElem->next = NULL;

		aElem->node = malloc(3 * sizeof(node_t *));
		for (int iNode = 0; iNode < 3; ++iNode) {
			aElem->node[iNode] = vertexPtr[tria[iTria][iNode]];
		}

		aElem->bary[X] = aElem->bary[Y] = 0;
		for (int iNode = 0; iNode < 3; ++iNode) {
			for (int i = 0; i < NDIM; ++i) {
				aElem->bary[i] += aElem->node[iNode]->x[i] / 3;
			}
		}

		aElem->firstSide = NULL;
		for (int iSide = 0; iSide < 3; ++iSide) {
			side_t *aSide = malloc(sizeof(side_t));
			aSide->connection = NULL;
			aSide->nextElemSide = NULL;
			aSide->next = NULL;
			aSide->BC = NULL;
			aSide->node[0] = aElem->node[iSide];
			aSide->node[1] = aElem->node[(iSide + 1) % 3];
			aSide->elem = aElem;

			aSide->nextElemSide = aElem->firstSide;
			aElem->firstSide = aSide;

			long iNode1 = aElem->node[iSide]->id;
			long iNode2 = aElem->node[(iSide + 1) % 3]->id;

			sideList[iSidePtr].node[0] = fmin(iNode1, iNode2);
			sideList[iSidePtr].node[1] = fmax(iNode1, iNode2);
			sideList[iSidePtr].BC = false;
			if (fmin(iNode1, iNode2) == iNode2) {
				sideList[iSidePtr].isRotated = true;
			} else {
				sideList[iSidePtr].isRotated = false;
			}
			sideList[iSidePtr].side = aSide;

			iSidePtr++;
		}
	}
	if (nTrias > 0) {
		free(tria);
	}

	/* loop over all quadrilaterals */
	for (long iQuad = 0; iQuad < nQuads; ++iQuad) {
		elem_t *aElem = malloc(sizeof(elem_t));
		aElem->id = iElem++;
		aElem->domain = quad[iQuad][4];
		aElem->elemType = 4;

		if (!firstElem) {
			firstElem = aElem;
			prevElem = aElem;
		} else {
			prevElem->next = aElem;
			prevElem = aElem;
		}
		aElem->next = NULL;

		aElem->node = malloc(4 * sizeof(node_t *));
		for (int iNode = 0; iNode < 4; ++iNode) {
			aElem->node[iNode] = vertexPtr[quad[iQuad][iNode]];
		}

		aElem->bary[X] = aElem->bary[Y] = 0;
		for (int iNode = 0; iNode < 4; ++iNode) {
			for (int i = 0; i < NDIM; ++i) {
				aElem->bary[i] += aElem->node[iNode]->x[i] / 4;
			}
		}

		aElem->firstSide = NULL;
		for (int iSide = 0; iSide < 4; ++iSide) {
			side_t *aSide = malloc(sizeof(side_t));
			aSide->connection = NULL;
			aSide->nextElemSide = NULL;
			aSide->next = NULL;
			aSide->BC = NULL;
			aSide->node[0] = aElem->node[iSide];
			aSide->node[1] = aElem->node[(iSide + 1) % 4];
			aSide->elem = aElem;

			aSide->nextElemSide = aElem->firstSide;
			aElem->firstSide = aSide;

			long iNode1 = aElem->node[iSide]->id;
			long iNode2 = aElem->node[(iSide + 1) % 4]->id;

			sideList[iSidePtr].node[0] = fmin(iNode1, iNode2);
			sideList[iSidePtr].node[1] = fmax(iNode1, iNode2);
			sideList[iSidePtr].BC = false;
			if (fmin(iNode1, iNode2) == iNode2) {
				sideList[iSidePtr].isRotated = true;
			} else {
				sideList[iSidePtr].isRotated = false;
			}
			sideList[iSidePtr].side = aSide;

			iSidePtr++;
		}

	}
	if (nQuads > 0) {
		free(quad);
	}

	/* sides and connectivity */
	/* save all BCedges into the sideList array */
	for (long iSide = 0; iSide < nBCsides; ++iSide) {
		side_t *aSide = malloc(sizeof(side_t));
		aSide->connection = NULL;
		aSide->nextElemSide = NULL;
		aSide->next = NULL;
		aSide->node[0] = vertexPtr[BCedge[iSide][0]];
		aSide->node[1] = vertexPtr[BCedge[iSide][1]];

		elem_t *aElem = malloc(sizeof(elem_t));
		aSide->elem = aElem;

		long iNode1 = vertexPtr[BCedge[iSide][0]]->id;
		long iNode2 = vertexPtr[BCedge[iSide][1]]->id;

		sideList[iSidePtr].node[0] = fmin(iNode1, iNode2);
		sideList[iSidePtr].node[1] = fmax(iNode1, iNode2);
		sideList[iSidePtr].BC = true;
		if (fmin(iNode1, iNode2) == iNode2) {
			sideList[iSidePtr].isRotated = true;
		} else {
			sideList[iSidePtr].isRotated = false;
		}

		int BCtype = BCedge[iSide][2] / 100;
		int BCid = BCedge[iSide][2] % 100;

		boundary_t *aBC = firstBC;
		while (aBC) {
			if ((aBC->BCtype == BCtype) && (aBC->BCid == BCid)) {
				aSide->BC = aBC;
				break;
			}
			aBC = aBC->next;
		}
		if ((BCtype != 0) && (!aBC)) {
			printf("| ERROR: BC %d from Gridgen Meshfile not defined in Parameter File\n", BCtype * 100 + BCid);
			exit(1);
		}

		sideList[iSidePtr].side = aSide;

		iSidePtr++;
	}
	free(BCedge);

	/*
	 * Sort sideList array in order to retreive the connectivity info more
	 * efficiently: basically we want to sort a 2D array, the first column
	 * is the first node of every side (sideList[i].node[0]) and the second
	 * column is the second node of every side (sideList[i].node[1]). The
	 * first column should be ordered in ascending order and for all the
	 * that are the same, the second column should be ordered in ascending
	 * order:
	 * 1 3       1 2
	 * 2 1       1 3
	 * 1 2  -->  2 1
	 * 3 1       2 2
	 * 2 2       3 1
	 * 3 3       3 3
	 */
	//for (long i = 0; i < 2*nSides; ++i) {
	//	printf("%5ld %5ld %5ld %d %c\n", i+1, sideList[i].node[0]+1, sideList[i].node[1]+1, sideList[i].BC, sideList[i].isRotated ? 'T' : 'F');
	//}
	//printf("\n");
	qsort(sideList, 2 * nSides, sizeof(sideList[0]), compare);
	//for (long i = 0; i < 2*nSides; ++i) {
	//	printf("%5ld %5ld %5ld %d %c\n", i+1, sideList[i].node[0]+1, sideList[i].node[1]+1, sideList[i].BC, sideList[i].isRotated ? 'T' : 'F');
	//}

	/* initialize side lists in mesh */
	firstSide = NULL;
	firstBCside = NULL;
	for (long iSide = 0; iSide < 2 * nSides; iSide += 2) {
		side_t *aSide = sideList[iSide].side;
		side_t *bSide = sideList[iSide + 1].side;

		/* grid file read check */
		if ((sideList[iSide].node[0] - sideList[iSide + 1].node[0] +
		     sideList[iSide].node[1] - sideList[iSide + 1].node[1])
				!= 0) {
			printf("| ERROR: Problem in Connect\n");
			exit(1);
		}

		/* save sides into global element array */
		if ((aSide->BC) || (bSide->BC)) {
			/* boundary side: make sure aSide is the physical side
			 * and bSide the ghost side and save only aSide into
			 * list	*/
			if (aSide->BC) {
				side_t *tmp = aSide;
				aSide = bSide;
				bSide = tmp;
			}

			/* save boundary side (bSide) into boundary side list */
			sidePtr_t *aBCside = malloc(sizeof(sidePtr_t));
			aBCside->side = bSide;
			aBCside->next = firstBCside;
			firstBCside = aBCside;
		}

		aSide->next = firstSide;
		firstSide = aSide;

		aSide->connection = bSide;
		bSide->connection = aSide;
	}

	/* extend element info: area and projection of cell onto axes */
	totalArea_q = 0.0;
	elem_t *aElem = firstElem;
	while (aElem) {
		createElemInfo(aElem);
		//printf("%g %g %g %g %g\n", aElem->bary[X], aElem->bary[Y], aElem->area, aElem->sx, aElem->sy);
		aElem = aElem->next;
	}

	/* generate virtual barycenters and ghostcells */
	sidePtr_t *aBCside = firstBCside;
	while (aBCside) {
		side_t *aSide = aBCside->side;
		double tmp = 2.0 *
			fabs(aSide->connection->GP[X] * aSide->connection->n[X] +
			     aSide->connection->GP[Y] * aSide->connection->n[Y]);
		aSide->elem->bary[X] = aSide->connection->elem->bary[X] + tmp +
			aSide->connection->n[X];
		aSide->elem->bary[Y] = aSide->connection->elem->bary[Y] + tmp +
			aSide->connection->n[Y];
		aBCside = aBCside->next;
	}

	/* extend side info: vector between barycenters, gaussian integration
	 * points and normal vectors */
	side_t *aSide = firstSide;
	while (aSide) {
		createSideInfo(aSide);
		aSide = aSide->next;
	}

	/* periodic BCs */
	connectPeriodicBC();

	/* variables for reconstruction */
	aElem = firstElem;
	while (aElem) {
		createReconstructionInfo(aElem);
		aElem = aElem->next;
	}

	/* element and side lists */
	side = malloc(nSides * sizeof(side_t *));
	elem = malloc(nElems * sizeof(elem_t *));
	BCside = malloc(nBCsides * sizeof(side_t *));

	aElem = firstElem;
	iElem = 0;
	while (aElem) {
		elem[iElem++] = aElem;
		aElem = aElem->next;
	}

	aSide = firstSide;
	long iSide = 0;
	while (aSide) {
		side[iSide++] = aSide;
		aSide = aSide->next;
	}

	aBCside = firstBCside;
	iSide = 0;
	while (aBCside) {
		if (aBCside->side->BC->BCtype != PERIODIC) {
			BCside[iSide++] = aBCside->side;
		}

		aBCside = aBCside->next;
	}
	nBCsides = iSide;
}

/*
 * read in and store the mesh parameters from parameter file
 */
void readMesh(void)
{
	meshType = getInt("meshType", "1"); /* default is cartesian */
	switch (meshType) {
	case UNSTRUCTURED:
		printf("| Mesh Type is UNSTRUCTURED\n");

		strcpy(strMeshFormat, getStr("meshFormat", NULL));
		printf("| Mesh File Format is %s\n", strMeshFormat);

		strcpy(strMeshFile, getStr("meshFile", NULL));
		printf("| Mesh File: %s%s\n", strMeshFile, strMeshFormat);

		strcat(strMeshFile, strMeshFormat);
		break;
	case CARTESIAN:
		printf("| Mesh Type is CARTESIAN\n");

		cartMesh.iMax = getInt("nElemsX", NULL);
		cartMesh.jMax = getInt("nElemsY", NULL);

		double *X0 = getDblArray("X0", NDIM, NULL);
		xMin = X0[0];
		yMin = X0[1];
		double *DX = getDblArray("Xmax", NDIM, NULL);
		xMax = DX[0];
		yMax = DX[1];

		/* read boundary conditions */
		cartMesh.nBC = getIntArray("nBCsegments", 2 * NDIM, NULL);
		for (int iSide = 0; iSide < 2 * NDIM; ++iSide) {
			if (cartMesh.nBC[iSide] == 1) {
				cartMesh.BCtype[iSide][0] = getInt("meshBCtype", NULL);
				cartMesh.BCrange[iSide][0][0] = 1;
				if ((iSide % 2) == 0) {
					cartMesh.BCrange[iSide][0][1] = cartMesh.iMax;
				} else {
					cartMesh.BCrange[iSide][0][1] = cartMesh.jMax;
				}
			} else {
				for (int i = 0; i < cartMesh.nBC[i]; ++i) {
					cartMesh.BCtype[iSide][i] = getInt("meshBCtype", NULL);
					cartMesh.BCrange[iSide][i][0] = getInt("BCstart", NULL);
					cartMesh.BCrange[iSide][i][1] = getInt("BCend", NULL);
				}
			}
		}
		break;
	default:
		printf("ERROR: Mesh type can only be unstructured(=0) or cartesian(=1)\n");
		exit(1);
	}
}

/*
 * Initalize the mesh and call read mesh
 */
void initMesh(void)
{
	printf("\nInitializing Mesh:\n");
	readMesh();
	strcat(strcpy(gridFile, strOutFile), "_mesh.cgns");
	createMesh();
	if ((iVisuProg == CGNS) && (!isRestart)) {
		//cgnsWriteMesh();
	}
	dxRef = sqrt(1.0 / (totalArea_q * nElems));
}