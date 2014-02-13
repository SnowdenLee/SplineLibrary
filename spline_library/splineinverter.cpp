#include "splineinverter.h"

#include "spline.h"
#include "spline_library/utils/splinesample_adaptor.h"
#include "spline_library/utils/nanoflann.hpp"

#include <algorithm>

#define PI 3.14159265359

template <typename T> int sign(T val) {
    return (T(0) < val) - (val < T(0));
}

//inner class used to provide an abstraction between the spline inverter and nanoflann
class SplineInverter::SampleTree
{
    typedef SplineSampleAdaptor<SplineSamples3D> AdaptorType;
    typedef nanoflann::KDTreeSingleIndexAdaptor<
            nanoflann::L2_Simple_Adaptor<double, AdaptorType>,
            AdaptorType, 3>
        TreeType;

public:
    SampleTree(const SplineSamples3D &samples)
        :adaptor(samples), tree(3, adaptor)
    {
        tree.buildIndex();
    }

    double findClosestSample(const Vector3D &queryPoint) const
    {
        //build a query point
        double queryPointArray[3] = {queryPoint.x(), queryPoint.y(), queryPoint.z()};

        // do a knn search
        const size_t num_results = 1;
        size_t ret_index;
        double out_dist_sqr;
        nanoflann::KNNResultSet<double> resultSet(num_results);
        resultSet.init(&ret_index, &out_dist_sqr );
        tree.findNeighbors(resultSet, &queryPointArray[0], nanoflann::SearchParams());

        return adaptor.derived().pts.at(ret_index).t;
    }

private:
    AdaptorType adaptor;
    TreeType tree;

};

SplineInverter::SplineInverter(const std::shared_ptr<Spline> &spline, int samplesPerT)
    :spline(spline), sampleStep(1.0 / double(samplesPerT)), slopeTolerance(0.01)
{
    SplineSamples3D samples;

	//first step is to populate the splineSamples map
	//we're going to have sampled T values sorted by x coordinate
	double currentT = 0;
	double maxT = spline->getMaxT();
	while(currentT < maxT)
	{
		auto sampledPoint = spline->getPosition(currentT);
        int test1 = samples.pts.size();

        SplineSamples3D::Point3D currentSample(sampledPoint.x(), sampledPoint.y(), sampledPoint.z(), currentT);
        samples.pts.push_back(currentSample);

		currentT += sampleStep;
	}

	//if the spline isn't a loop and the final t value isn't very very close to maxT, we have to add a sample for maxT
    double lastT = samples.pts.at(samples.pts.size() - 1).t;
    if(!spline->isLooping() && abs(lastT / maxT - 1) > .0001)
	{
		auto sampledPoint = spline->getPosition(maxT);

        SplineSamples3D::Point3D currentSample(sampledPoint.x(), sampledPoint.y(), sampledPoint.z(), maxT);
        samples.pts.push_back(currentSample);
    }

    //populate the sample kd-tree
    sampleTree = std::unique_ptr<SampleTree>(new SampleTree(samples));
}

SplineInverter::~SplineInverter()
{

}

double SplineInverter::findClosestFast(const Vector3D &queryPoint) const
{
    double closestSampleT = sampleTree->findClosestSample(queryPoint);

	double sampleDistanceSlope = getDistanceSlope(queryPoint, closestSampleT);

	//if the slope is very close to 0, just return the sampled point
	if(abs(sampleDistanceSlope) < slopeTolerance)
		return closestSampleT;

	//if the spline is not a loop there are a few special cases to account for
    if(!spline->isLooping())
	{
		//if closest sample T is 0, we are on an end. so if the slope is positive, we have to just return the end
		if(abs(closestSampleT) < .0001 && sampleDistanceSlope > 0)
			return closestSampleT;

		//if the closest sample T is max T we are on an end. so if the slope is negative, just return the end
		if(abs(closestSampleT / spline->getMaxT() - 1) < .0001 && sampleDistanceSlope < 0)
			return closestSampleT;
	}


	//step forwards or backwards in the spline until we find a point where the distance slope has flipped sign
	//because "currentsample" is the closest point,  the "next" sample's slope MUST have a different sign
	//otherwise that sample would be closer
	//note: this assumption is only true if the samples are close together

	double bigT, littleT;
	if(sampleDistanceSlope > 0)
	{
		//the distance slope is < 0, so the spline is moving towards the query point at closestSampleT
		bigT = closestSampleT;
		littleT = closestSampleT - sampleStep;
	}
	else
	{
		//the distance slope is < 0, so the spline is moving towards the query point at closestSampleT
		bigT = closestSampleT + sampleStep;
		littleT = closestSampleT;
	}

	//we know that the actual closest point is now between littleT and bigT
	//use the circle projection method to find the actual closest point, using the Ts as bounds
	return circleProjectionMethod(queryPoint, littleT, bigT);
}

double SplineInverter::findClosestPrecise(const Vector3D &queryPoint) const
{
    double closestSampleT = sampleTree->findClosestSample(queryPoint);

	double sampleDistanceSlope = getDistanceSlope(queryPoint, closestSampleT);

	//if the slope is very close to 0, just return the sampled point
	if(abs(sampleDistanceSlope) < slopeTolerance)
		return closestSampleT;

	//if the spline is not a loop there are a few special cases to account for
    if(!spline->isLooping())
	{
		//if closest sample T is 0, we are on an end. so if the slope is positive, we have to just return the end
		if(abs(closestSampleT) < .0001 && sampleDistanceSlope > 0)
			return closestSampleT;

		//if the closest sample T is max T we are on an end. so if the slope is negative, just return the end
		if(abs(closestSampleT / spline->getMaxT() - 1) < .0001 && sampleDistanceSlope < 0)
			return closestSampleT;
	}

	
	//step forwards or backwards in the spline until we find a point where the distance slope has flipped sign
	//because "currentsample" is the closest point,  the "next" sample's slope MUST have a different sign
	//otherwise that sample would be closer
	//note: this assumption is only true if the samples are close together

	double bigT, littleT;
	if(sampleDistanceSlope > 0)
	{
		//the distance slope is < 0, so the spline is moving towards the query point at closestSampleT
		bigT = closestSampleT;
		littleT = closestSampleT - sampleStep;
	}
	else
	{
		//the distance slope is < 0, so the spline is moving towards the query point at closestSampleT
		bigT = closestSampleT + sampleStep;
		littleT = closestSampleT;
	}

	//we know that the actual closest point is now between littleT and bigT
	//use the circle projection method to find the actual closest point, using the Ts as bounds
	return bisectionMethod(queryPoint, littleT, bigT);
}

double SplineInverter::bisectionMethod(const Vector3D &queryPoint, double lowerBound, double upperBound) const
{
	//we need to use the bisection method until the slope of the distance is 0.
        //when this happens we have found a local minimum or global minimum
	double lowerValue = getDistanceSlope(queryPoint, lowerBound);
	double upperValue = getDistanceSlope(queryPoint, upperBound);

	//make sure a has y<0 and b has y>0. if its the other way around, swap them
	if(lowerValue > upperValue)
	{
		auto tempValue = lowerValue;
		lowerValue = upperValue;
		upperValue = tempValue;

		auto temp = lowerBound;
		lowerBound = upperBound;
		upperBound = temp;
	}

	//use the bisection method (essentially binary search) to find the value
	double middle = (lowerBound + upperBound) * 0.5;
	double middleValue = getDistanceSlope(queryPoint, middle);
	while(abs(middleValue) > slopeTolerance)
	{
		if(middleValue > 0)
		{
			upperBound = middle;
			upperValue = middleValue;
		}
		else
		{
			lowerBound = middle;
			lowerValue = middleValue;
		}

		middle = (lowerBound + upperBound) * 0.5;
		middleValue = getDistanceSlope(queryPoint, middle);
	}

	return middle;
}


double SplineInverter::circleProjectionMethod(const Vector3D &queryPoint, double lowerBound, double upperBound) const
{
	//long story short: we're going to project the query point on a circle arc
	//that travels between the position at lowerBound, and the position at upperBound

	//the radius of the circle will be determined by the angle between the tangents
	//at lowerBound and upperBound
	
	//if the angle is very small then the radius will be very large which 
	//will lead to the arc behaving like a straight line
	//if the angle is very large then the radius will be smaller which will
	//result in the t value being projected onto a more curved surface

	//the idea is that the curve will roughly match the curve of the actual spline

	//there are more accurate calculation methods like the bisection method,
	//but this is a reasonable approximation and way faster

    auto lowerResult = spline->getTangent(lowerBound);
    auto upperResult = spline->getTangent(upperBound);

    Vector3D sampleDisplacement = upperResult.position - lowerResult.position;
    Vector3D queryDisplacement = queryPoint - lowerResult.position;

    double sampleLength = sampleDisplacement.length();
    Vector3D sampleDirection = sampleDisplacement / sampleLength;

    //project the query vector onto the sample vector
    double projection = Vector3D::dotProduct(sampleDirection, queryDisplacement);

    //compute the angle between the before and after velocities
    Vector3D lowerTangentDirection = lowerResult.tangent.normalized();
    Vector3D upperTangentDirection = upperResult.tangent.normalized();
    double cosTangentAngle = Vector3D::dotProduct(upperTangentDirection, lowerTangentDirection);

    //if the cos(angle) between the velocities is almost 1, there is essentially a straight line between the two points
    //if that's the case, we should just project the point onto that line
    if(cosTangentAngle > .99999)
    {
        double percent = projection / sampleLength;
        return lowerBound + percent * (upperBound - lowerBound);
    }
    else
    {
        //reject the projection to get a vector perpendicular to the sample vector out to the query point
        Vector3D queryRejection = queryDisplacement - projection * sampleDirection;

        //find the dot product between the rejection and the average acceleration, both non-normalized
        Vector3D averageCurvature = upperResult.tangent - lowerResult.tangent;
        double curvatureDot = Vector3D::dotProduct(averageCurvature,queryRejection);



        //construct an isoceles tringle whose base is the sample displacement, and whose opposite angle is the velocity angle
        double arcAngle = acos(cosTangentAngle);
        double h = sampleLength * 0.5 / tan(arcAngle * 0.5);

        //find the apex point
        Vector3D apex = (lowerResult.position + upperResult.position) * 0.5 + queryRejection.normalized() * h * sign(curvatureDot);

        //find the vectors that go from apex to the sample point, and from apex to the query point
        Vector3D sampleVector = lowerResult.position - apex;
        Vector3D queryVector = queryPoint - apex;

        //find the angle between these
        double queryAngle = acos(Vector3D::dotProduct(sampleVector.normalized(), queryVector.normalized()));

        double percent = queryAngle / arcAngle;
        return lowerBound + percent * (upperBound - lowerBound);
    }
}

double SplineInverter::getDistanceSlope(const Vector3D &queryPoint, double t) const
{
    auto result = spline->getTangent(t);

	//get the displacement from the spline at T to the query point
	Vector3D displacement = result.position - queryPoint;

	//find projection of spline velocity onto displacement
    return Vector3D::dotProduct(displacement.normalized(), result.tangent);
}
