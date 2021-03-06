/****************************************************************/
/*               DO NOT MODIFY THIS HEADER                      */
/* MOOSE - Multiphysics Object Oriented Simulation Environment  */
/*                                                              */
/*           (c) 2010 Battelle Energy Alliance, LLC             */
/*                   ALL RIGHTS RESERVED                        */
/*                                                              */
/*          Prepared by Battelle Energy Alliance, LLC           */
/*            Under Contract No. DE-AC07-05ID14517              */
/*            With the U. S. Department of Energy               */
/*                                                              */
/*            See COPYRIGHT for full restrictions               */
/****************************************************************/

#include "EventMarker.h"

template<>
InputParameters validParams<EventMarker>()
{
  InputParameters params = validParams<Marker>();

  params.addRequiredParam<UserObjectName>("inserter", "The name of the EventInserter UserObject.");
  params.addRequiredParam<UserObjectName>("gaussian_user_object", "The name of the GaussianUserObject.");
  params.addParam<Real>("marker_radius", 3.0, "How many sigmas to mark away from event center.");
  params.addParam<bool>("verbose", false, "Print out extra information when marker is active.");
  params.addCoupledVar("periodic_variable", "Use perodic boundary conditions of this variable to determine the distance to the function peak location");
  params.addParam<bool>("coarsen_events", false, "Coarsen events at some later time. If true, set 'track_old_events=true' in EventInserter.");
  params.addParam<Real>("event_sigma_mesh_ratio", 2.0, "Refine elements until ratio of event sigma to element size is equal to or greater than this value.");
  params.addParam<bool>("refine_sinks", false, "Refine the area around sinks from SinkMapUserObject.");
  params.addParam<Real>("sink_marker_radius", 3.0, "How many sigmas to mark away from sink center.");
  params.addParam<Real>("sink_sigma_mesh_ratio", 2.0, "Refine elements until ratio of sink sigma to element size is equal to or greater than this value.");
  params.addParam<UserObjectName>("sink_map_user_object", "The name of the SinkMapUserObject.");
  params.addParam<UserObjectName>("sink_gaussian_user_object", "The name of the GaussianUserObject to use for sink size.");

  return params;
}


EventMarker::EventMarker(const InputParameters & parameters) :
    Marker(parameters),
    Coupleable(this, false),
    _inserter(getUserObject<EventInserterBase>("inserter")),
    _gaussian_uo(getUserObject<GaussianUserObject>("gaussian_user_object")),
    _marker_radius(getParam<Real>("marker_radius")),
    _verbose(getParam<bool>("verbose")),
    _periodic_var(isCoupled("periodic_variable") ? (int) coupled("periodic_variable") : -1),
    _coarsen_events(getParam<bool>("coarsen_events")),
    _sigma_mesh_ratio(getParam<Real>("event_sigma_mesh_ratio")),
    _refine_distance(_marker_radius * _gaussian_uo.getSigma()),
    _minimum_element_size(_gaussian_uo.getSigma() / _sigma_mesh_ratio),
    _refine_by_ratio(parameters.isParamSetByUser("event_sigma_mesh_ratio")),
    _refine_sinks(getParam<bool>("refine_sinks")),
    _sink_marker_radius(getParam<Real>("sink_marker_radius")),
    _refine_sinks_by_ratio(parameters.isParamSetByUser("sink_sigma_mesh_ratio")),
    _sink_sigma_mesh_ratio(getParam<Real>("sink_sigma_mesh_ratio")),
    _uniform_refinement_level(_mesh.uniformRefineLevel()),
    _sink_map_user_object_ptr(NULL),
    _sink_gaussian_user_object_ptr(NULL),
    _event_incoming(false),
    _event_location(0),
    _coarsening_needed(false),
    _sink_refinement_needed(false),
    _old_event_list(0)
{
  // Check input logic for coarsening events
  if ((_coarsen_events) && (!_inserter.areOldEventsBeingTracked())) // need to tell EventInserter to track old events
    mooseError("When coarsening old events ('coarsen_events = true'), EventInserter object needs to track old events. Please set 'track_old_events = true' in EventInserter block.");

  // Check input logic for refining around sinks
  if (_refine_sinks)
  {
    if ((parameters.isParamSetByUser("sink_map_user_object")) && (parameters.isParamSetByUser("sink_gaussian_user_object")))
    {
      _sink_map_user_object_ptr = &getUserObject<SinkMapUserObject>("sink_map_user_object");
      _sink_gaussian_user_object_ptr = &getUserObject<GaussianUserObject>("sink_gaussian_user_object");
      _sink_refine_distance = _sink_marker_radius * _sink_gaussian_user_object_ptr->getSigma();
      if (_refine_sinks_by_ratio)
        _minimum_sink_element_size = _sink_gaussian_user_object_ptr->getSigma() / _sink_sigma_mesh_ratio;
    }
    else if (!parameters.isParamSetByUser("sink_map_user_object"))
      mooseError("To refine around sinks, need to set 'sink_map_user_object' to the name of the SinkMapUserObject.");
    else
      mooseError("To refine around sinks, need to set 'sink_gaussian_user_object' to the name of the GaussianUserObject for sinks.");
  }
}

void
EventMarker::markerSetup()
{
  // default to no adaptivity
  _event_incoming = false;
  _coarsening_needed = false;
  _sink_refinement_needed = false;

  // Check if event will occur during the NEXT timestep, so we can refine now and be ready for it
  // Event will be active on the next time step if we are at the next event time NOW
  if (_inserter.isEventActive(_fe_problem.time() - _inserter.getTimeTolerance(), _fe_problem.time() + _inserter.getTimeTolerance()))  // fuzzy math comparing floats
  {
    _event_location = _inserter.getActiveEventPoint(_fe_problem.time() - _inserter.getTimeTolerance(), _fe_problem.time() + _inserter.getTimeTolerance());
    _event_incoming = true;
    if (_verbose)
      _console << "Event incoming! location: " << _event_location << std::endl;
  }

  // coarsen if requested and if old event was removed
  if ((_coarsen_events) && (_inserter.wasOldEventRemoved()))
  {
    // get old event list to know where to NOT coarsen
    _old_event_list = _inserter.getOldEventList();
    _coarsening_needed = true;
    if (_verbose)
      _console << "EventMarker detected an old event was removed, coarsening mesh..." << std::endl;
  }

  // refine around sinks initially
  if ((_refine_sinks) && (_fe_problem.timeStep() == 0))
    _sink_refinement_needed = true;

  // might need to re-refine around sinks if coarsening is happening to a nearby event
  if ((_refine_sinks) && (_coarsening_needed))
    _sink_refinement_needed = true;
}

Marker::MarkerValue
EventMarker::computeElementMarker()
{
  // default marker value
  MarkerValue marker_value = DO_NOTHING;

  // get centroid for this element
  // optionally do qp's
  Point centroid = _current_elem->centroid();

  // refine mesh if event is incoming
  if (_event_incoming)
  {
    // distance to center depends on perodicity
    Real r;
    if (_periodic_var < 0)
      r = (_event_location - centroid).norm();
    else
      r = _mesh.minPeriodicDistance(_periodic_var, _event_location, centroid);

    if ((r < _refine_distance) || (_current_elem->contains_point(_event_location))) // we are near the event or element contains event
    {
      if (!_refine_by_ratio)  // refine if distance is the only critereon
        marker_value = REFINE;
      else if (_current_elem->hmax() > _minimum_element_size) // or if screening by element size, check the element size
        marker_value = REFINE;
    }
  }

  if (_coarsening_needed)
  {
    // default to coarsen
    marker_value = COARSEN;

    // coarsen everywhere except inside old events
    for (unsigned int i=0; i<_old_event_list.size(); i++)
    {
      Point old_event_location = _old_event_list[i].second;
      // TODO: remove code duplication
      // distance to center depends on perodicity
      Real r;
      if (_periodic_var < 0)
        r = (old_event_location - centroid).norm();
      else
        r = _mesh.minPeriodicDistance(_periodic_var, old_event_location, centroid);

      if (r < _refine_distance)
        marker_value = DO_NOTHING;
    }
  }

  if (_sink_refinement_needed)
  {
    // get distance to nearest sink
    Real r = _sink_map_user_object_ptr->getDistanceToNearestSink(centroid);

    // get location of nearest sink
    Point nearest_sink = _sink_map_user_object_ptr->getNearestSinkLocation(centroid);

    // refine if close enough
    if ((r < _sink_refine_distance) || (_current_elem->contains_point(nearest_sink))) // we are near a sink or element contains the nearest sink
    {
      if (marker_value == COARSEN)
      {
        // if we made it here then sinks have already been refined (because events have already come and gone) and
        // we don't want to coarsen the mesh near a sink
        marker_value = DO_NOTHING;
        // unless the mesh is *too* dense near the sink, which can happen if the minimum event element size is less than mininum sink element size
        // here we will assume each level of refinement cuts hmax by 2, so we check if element size is less than that
        if ((_refine_sinks_by_ratio) && (_current_elem->hmax() < _minimum_sink_element_size/2.0))
          marker_value = COARSEN;
      }
      else if (!_refine_sinks_by_ratio)  // refine if distance is the only critereon
        marker_value = REFINE;
      else if (_current_elem->hmax() > _minimum_sink_element_size) // or if screening by element size, check the element size
        marker_value = REFINE;
    }
  }

  // don't bother trying to coarsen an element that is already at level 0
  // plus it will make the marker field easier to interpret
  if ((marker_value == COARSEN) && (_current_elem->level() == 0))
    marker_value = DO_NOTHING;

  // one last check to make sure we are not coarsening beyond the applied uniform refinement
  // the last expression is to leave it alone when no intervening is needed
  if ((marker_value == COARSEN) && (_current_elem->level() == _uniform_refinement_level) && (_uniform_refinement_level > 0))
    marker_value = DO_NOTHING;

  return marker_value;
}
