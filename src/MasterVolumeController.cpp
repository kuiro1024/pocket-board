#include "MasterVolumeController.h"

#include <QtGlobal>

bool MasterVolumeController::setValue(int v)
{
    const int bounded = qBound(0, v, 100);
    if (bounded == m_value)
        return false;
    m_value = bounded;
    return true;
}

bool MasterVolumeController::applyWheelSteps(int steps)
{
    return setValue(m_value + steps);
}
