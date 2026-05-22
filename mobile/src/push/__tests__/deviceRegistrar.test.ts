/**
 * TDD layer M3.a — DeviceRegistrar.
 */
import {DeviceRegistrar} from '../deviceRegistrar';

describe('DeviceRegistrar.ensureRegistered', () => {
  it('registers the token with the cloud (sipUsername + platform)', async () => {
    const register = jest.fn(async () => {});
    const registrar = new DeviceRegistrar(register, 'a101', 'ios');

    await registrar.ensureRegistered('tok-1');

    expect(register).toHaveBeenCalledWith({
      sipUsername: 'a101',
      platform: 'ios',
      token: 'tok-1',
    });
  });

  it('does not re-register the same token', async () => {
    const register = jest.fn(async () => {});
    const registrar = new DeviceRegistrar(register, 'a101', 'android');

    await registrar.ensureRegistered('tok-1');
    await registrar.ensureRegistered('tok-1');

    expect(register).toHaveBeenCalledTimes(1);
  });

  it('re-registers when the token rotates', async () => {
    const register = jest.fn(async () => {});
    const registrar = new DeviceRegistrar(register, 'a101', 'ios');

    await registrar.ensureRegistered('tok-1');
    await registrar.ensureRegistered('tok-2');

    expect(register).toHaveBeenCalledTimes(2);
    expect(register).toHaveBeenLastCalledWith({
      sipUsername: 'a101',
      platform: 'ios',
      token: 'tok-2',
    });
  });

  it('ignores an empty token', async () => {
    const register = jest.fn(async () => {});
    const registrar = new DeviceRegistrar(register, 'a101', 'ios');

    await registrar.ensureRegistered('');

    expect(register).not.toHaveBeenCalled();
  });

  it('does not remember a token whose registration failed — it retries', async () => {
    const register = jest
      .fn()
      .mockRejectedValueOnce(new Error('network'))
      .mockResolvedValueOnce(undefined);
    const registrar = new DeviceRegistrar(register, 'a101', 'ios');

    await expect(registrar.ensureRegistered('tok-1')).rejects.toThrow();
    await registrar.ensureRegistered('tok-1'); // retry succeeds

    expect(register).toHaveBeenCalledTimes(2);
  });
});
